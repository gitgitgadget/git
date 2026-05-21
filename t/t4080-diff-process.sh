#!/bin/sh

test_description='diff process via long-running process'

. ./test-lib.sh

if test_have_prereq PYTHON
then
	PYTHON_PATH=$(command -v python3) || PYTHON_PATH=$(command -v python)
fi

#
# A single parametric diff process.
# Usage: diff-process-backend --mode=<mode> [--log=<path>]
#
# Modes:
#   whole-file  - report all lines as changed (default)
#   fixed-hunk  - always report hunk 5 2 5 2
#   bad-hunk    - report out-of-bounds hunk 999 1 999 1
#   zero-hunk   - return zero hunks (files considered equivalent)
#   error       - return status=error for every request
#   abort       - return status=abort for every request
#   crash       - read one request then exit without responding
#
setup_backend () {
	cat >"$TRASH_DIRECTORY/diff-process-backend.py" <<-\PYEOF
	import sys, os

	def read_pkt():
	    hdr = sys.stdin.buffer.read(4)
	    if len(hdr) < 4: return None
	    length = int(hdr, 16)
	    if length == 0: return ""
	    data = sys.stdin.buffer.read(length - 4)
	    return data.decode().rstrip("\n")

	def write_pkt(line):
	    data = (line + "\n").encode()
	    sys.stdout.buffer.write(f"{len(data)+4:04x}".encode() + data)
	    sys.stdout.buffer.flush()

	def write_flush():
	    sys.stdout.buffer.write(b"0000")
	    sys.stdout.buffer.flush()

	def read_content():
	    chunks = []
	    while True:
	        hdr = sys.stdin.buffer.read(4)
	        if len(hdr) < 4: break
	        length = int(hdr, 16)
	        if length == 0: break
	        chunks.append(sys.stdin.buffer.read(length - 4))
	    return b"".join(chunks)

	mode = "whole-file"
	logfile = None
	for arg in sys.argv[1:]:
	    if arg.startswith("--mode="):
	        mode = arg[7:]
	    elif arg.startswith("--log="):
	        logfile = open(arg[6:], "a")

	def log(msg):
	    if logfile:
	        logfile.write(msg + "\n")
	        logfile.flush()

	# Handshake
	assert read_pkt() == "git-diff-client"
	assert read_pkt() == "version=1"
	read_pkt()
	write_pkt("git-diff-server")
	write_pkt("version=1")
	write_flush()
	while True:
	    p = read_pkt()
	    if p == "": break
	write_pkt("capability=hunks")
	write_flush()

	log("ready")

	while True:
	    cmd = None
	    pathname = None
	    while True:
	        p = read_pkt()
	        if p is None: sys.exit(0)
	        if p == "": break
	        if p.startswith("command="): cmd = p.split("=",1)[1]
	        if p.startswith("pathname="): pathname = p.split("=",1)[1]
	    if cmd is None: sys.exit(0)
	    old = read_content()
	    new = read_content()
	    log(f"command={cmd} pathname={pathname}")

	    if mode == "error":
	        write_flush()
	        write_pkt("status=error")
	        write_flush()
	        continue

	    if mode == "abort":
	        write_flush()
	        write_pkt("status=abort")
	        write_flush()
	        continue

	    if mode == "crash":
	        sys.exit(1)

	    if cmd == "hunks":
	        if mode == "fixed-hunk":
	            write_pkt("hunk 5 2 5 2")
	        elif mode == "bad-hunk":
	            write_pkt("hunk 999 1 999 1")
	        elif mode == "zero-hunk":
	            pass
	        else:
	            ol = len(old.split(b"\n"))
	            nl = len(new.split(b"\n"))
	            write_pkt(f"hunk 1 {ol} 1 {nl}")
	        write_flush()
	        write_pkt("status=success")
	        write_flush()
	    else:
	        write_flush()
	        write_pkt("status=error")
	        write_flush()
	PYEOF
	write_script diff-process-backend <<-SHEOF
	exec "$PYTHON_PATH" "$TRASH_DIRECTORY/diff-process-backend.py" "\$@"
	SHEOF
}

BACKEND="./diff-process-backend"

test_expect_success PYTHON 'setup' '
	setup_backend &&
	echo "*.c diff=cdiff" >.gitattributes &&
	git add .gitattributes &&
	git commit -m "initial"
'

test_expect_success PYTHON 'diff process hunk boundaries affect output' '
	cat >boundary.c <<-\EOF &&
	line1
	line2
	line3
	line4
	OLD5
	OLD6
	line7
	line8
	OLD9
	OLD10
	EOF
	git add boundary.c &&
	git commit -m "add boundary.c" &&

	cat >boundary.c <<-\EOF &&
	line1
	line2
	line3
	line4
	NEW5
	NEW6
	line7
	line8
	NEW9
	NEW10
	EOF

	# The file has changes at lines 5-6 and 9-10, but fixed-hunk
	# only reports lines 5-6 as changed.  Lines 9-10 should not
	# appear as changed in the output.
	git -c diff.cdiff.process="$BACKEND --mode=fixed-hunk" \
		diff boundary.c >actual &&
	grep "^-OLD5" actual &&
	grep "^-OLD6" actual &&
	grep "^+NEW5" actual &&
	grep "^+NEW6" actual &&
	! grep "^-OLD9" actual &&
	! grep "^-OLD10" actual &&
	! grep "^+NEW9" actual &&
	! grep "^+NEW10" actual
'

test_expect_success PYTHON 'diff process fallback on tool error status' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=error --log=backend.log" \
		diff boundary.c >actual &&
	# Fallback produces the full builtin diff (both change regions).
	grep "^-OLD5" actual &&
	grep "^+NEW5" actual &&
	grep "^-OLD9" actual &&
	grep "^+NEW9" actual &&
	# Tool was contacted (it replied with error, not crash).
	grep "command=hunks pathname=boundary.c" backend.log
'

test_expect_success PYTHON 'diff process fallback on bad hunks' '
	git -c diff.cdiff.process="$BACKEND --mode=bad-hunk" \
		diff boundary.c >actual &&
	grep "^-OLD5" actual &&
	grep "^+NEW5" actual &&
	grep "^-OLD9" actual &&
	grep "^+NEW9" actual
'

test_expect_success PYTHON 'diff process fallback on tool crash' '
	git -c diff.cdiff.process="$BACKEND --mode=crash" \
		diff boundary.c >actual &&
	grep "^-OLD5" actual &&
	grep "^+NEW5" actual &&
	grep "^-OLD9" actual &&
	grep "^+NEW9" actual
'

test_expect_success PYTHON 'diff process abort disables for session' '
	cat >abort1.c <<-\EOF &&
	int first(void) { return 1; }
	EOF
	cat >abort2.c <<-\EOF &&
	int second(void) { return 2; }
	EOF
	git add abort1.c abort2.c &&
	git commit -m "add abort files" &&

	cat >abort1.c <<-\EOF &&
	int first(void) { return 10; }
	EOF
	cat >abort2.c <<-\EOF &&
	int second(void) { return 20; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --mode=abort --log=backend.log" \
		diff -- abort1.c abort2.c >actual &&
	# Both files should still produce diff output via fallback.
	grep "return 10" actual &&
	grep "return 20" actual &&
	# The tool aborts on the first file and git clears its
	# capability.  The second file never contacts the tool,
	# so the log should have exactly one entry, not two.
	grep "command=hunks" backend.log >matches &&
	test_line_count = 1 matches
'

test_expect_success PYTHON 'diff process handles multiple files' '
	cat >multi1.c <<-\EOF &&
	int one(void) { return 1; }
	EOF
	cat >multi2.c <<-\EOF &&
	int two(void) { return 2; }
	EOF
	git add multi1.c multi2.c &&
	git commit -m "add multi files" &&

	cat >multi1.c <<-\EOF &&
	int one(void) { return 10; }
	EOF
	cat >multi2.c <<-\EOF &&
	int two(void) { return 20; }
	EOF

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff -- multi1.c multi2.c >actual &&
	grep "return 10" actual &&
	grep "return 20" actual &&
	grep "pathname=multi1.c" backend.log &&
	grep "pathname=multi2.c" backend.log
'

test_expect_success PYTHON 'diff process with --word-diff' '
	cat >worddiff.c <<-\EOF &&
	int value(void) { return 1; }
	EOF
	git add worddiff.c &&
	git commit -m "add worddiff.c" &&

	cat >worddiff.c <<-\EOF &&
	int value(void) { return 999; }
	EOF

	git -c diff.cdiff.process="$BACKEND" \
		diff --word-diff worddiff.c >actual &&
	grep "\[-1;-\]" actual &&
	grep "{+999;+}" actual
'

test_expect_success PYTHON 'diff process bypassed by --diff-algorithm' '
	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		diff --diff-algorithm=patience worddiff.c >actual &&
	grep "return 999" actual &&
	test_path_is_missing backend.log
'

test_expect_success PYTHON 'diff process works with git log -p' '
	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 1; }
	EOF
	git add logtest.c &&
	git commit -m "add logtest.c" &&

	cat >logtest.c <<-\EOF &&
	int logfunc(void) { return 2; }
	EOF
	git add logtest.c &&
	git commit -m "change logtest.c" &&

	rm -f backend.log &&
	git -c diff.cdiff.process="$BACKEND --log=backend.log" \
		log -1 -p -- logtest.c >actual &&
	grep "return 2" actual &&
	grep "command=hunks pathname=logtest.c" backend.log
'

test_expect_success PYTHON 'diff process zero hunks suppresses diff output' '
	cat >zerohunk.c <<-\EOF &&
	int zero(void) { return 0; }
	EOF
	git add zerohunk.c &&
	git commit -m "add zerohunk.c" &&

	cat >zerohunk.c <<-\EOF &&
	int zero(void) { return 999; }
	EOF

	git -c diff.cdiff.process="$BACKEND --mode=zero-hunk" \
		diff zerohunk.c >actual &&
	test_must_be_empty actual
'

test_expect_success PYTHON 'blame skips commits with zero hunks from diff process' '
	cat >blame.c <<-\EOF &&
	int main(void)
	{
	    return 0;
	}
	EOF
	git add blame.c &&
	git commit -m "add blame.c" &&

	cat >blame.c <<-\EOF &&
	int main(void)
	{
	        return 0;
	}
	EOF
	git add blame.c &&
	git commit -m "reformat blame.c" &&
	BLAME_COMMIT=$(git rev-parse --short HEAD) &&

	# Without zero-hunk mode, blame attributes the change.
	git blame blame.c >without &&
	grep "$BLAME_COMMIT" without &&

	# With zero-hunk mode, the process considers the files equivalent
	# and blame skips the reformat commit.
	git -c diff.cdiff.process="$BACKEND --mode=zero-hunk" \
		blame blame.c >with &&
	! grep "$BLAME_COMMIT" with
'

NORMALIZE="git diff-process-normalize"

test_expect_success 'diff-process-normalize setup' '
	echo "*.c diff=cdiff" >.gitattributes &&
	git add .gitattributes &&
	test_commit normalize-base
'

test_expect_success 'diff-process-normalize suppresses whitespace-only changes' '
	cat >ws.c <<-\EOF &&
	int main(void)
	{
	    return 0;
	}
	EOF
	git add ws.c &&
	git commit -m "add ws.c" &&

	cat >ws.c <<-\EOF &&
	int main(void)
	{
	        return 0;
	}
	EOF

	git -c diff.cdiff.process="$NORMALIZE" \
		diff ws.c >actual &&
	test_must_be_empty actual
'

test_expect_success 'diff-process-normalize falls back on non-whitespace changes' '
	cat >ws.c <<-\EOF &&
	int main(void)
	{
	    return 0;
	}

	int added_function(void)
	{
	    return 99;
	}
	EOF

	git -c diff.cdiff.process="$NORMALIZE" \
		diff ws.c >actual &&
	grep "added_function" actual
'

test_expect_success 'diff-process-normalize falls back on mixed whitespace and real changes' '
	cat >ws.c <<-\EOF &&
	int main(void)
	{
	        return 42;
	}
	EOF

	git -c diff.cdiff.process="$NORMALIZE" \
		diff ws.c >actual &&
	grep "return 42" actual
'

test_done
