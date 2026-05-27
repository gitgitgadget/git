import { ChildProcess } from "child_process";
import * as path from "path";
import { exec as GitProcess } from "dugite";

// For convenience, let's add helpers to call Git:

export interface IGitOptions {
    lineHandler?: (line: string) => Promise<void>;
    processCallback?: (process: ChildProcess) => void;
    stdin?: string | Buffer;
    workDir?: string;
    trimTrailingNewline?: boolean; // defaults to true
    trace?: boolean;
    env?: NodeJS.ProcessEnv;
}

export const emptyBlobName = "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391";
export const emptyTreeName = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";

function validateWorkDir(workDir: string): void {
    const prefix = process.env.GIT_WORK_DIR_PREFIX;
    if (!prefix) return;
    const gitDir = process.env.GIT_DIR;
    if (gitDir) {
        throw new Error(`GIT_DIR is set to '${gitDir}'; This should not happen in tests`);
    }
    if (!path.resolve(workDir).startsWith(prefix)) {
        throw new Error(`workDir '${workDir}' is not inside '${prefix}'`);
    }
}

function trimTrailingNewline(str: string): string {
    return str.replace(/\r?\n$/, "");
}

/**
 * Spawn a Git process with the given arguments.
 *
 * Note that the `workDir` attribute of the `options` can be used to specify
 * the working directory in which to run the command. If it is not specified,
 * the current working directory will be used.
 *
 * It can also be used to specify the path to a bare repository. This is only
 * possible, though, if the specified path ends with `.git`. Otherwise, this
 * function will assume that the repository is non-bare.
 *
 * @param {string[]} args the arguments to pass to `git`
 * @param {IGitOptions} options the options to pass to `dugite`
 * @returns {Promise<string>} the output of the command
 * @throws {Error} if the command fails
 */
export function git(args: string[], options?: IGitOptions): Promise<string> {
    let workDir = (options && options.workDir) || ".";
    // allow the command to run in a bare repository
    if (options?.workDir?.endsWith(".git")) {
        args = [`--git-dir=${options.workDir}`, ...args];
        workDir = ".";
    }
    const prefix = process.env.GIT_WORK_DIR_PREFIX;
    if (prefix) {
        let cmdStart = 0;

        // --git-dir=<path> must be the very first argument (prepended by the bare-repo handling above)
        if (args[0]?.startsWith("--git-dir=")) {
            validateWorkDir(args[0].substring("--git-dir=".length));
            cmdStart = 1;
        }

        if (args[cmdStart] === "init") {
            // Strictly parse: init [--bare] [--initial-branch <name>] <directory>
            let i = cmdStart + 1;
            while (i < args.length) {
                if (args[i] === "--bare") {
                    i++;
                } else if (args[i] === "--initial-branch" || args[i] === "-b") {
                    if (i + 1 >= args.length) throw new Error(`git init: '${args[i]}' requires a value`);
                    i += 2;
                } else if (args[i].startsWith("-")) {
                    throw new Error(`git init: unexpected option '${args[i]}'`);
                } else {
                    break;
                }
            }
            if (i >= args.length) {
                throw new Error(`git init without an explicit target directory is not allowed in tests`);
            }
            if (i + 1 < args.length) {
                throw new Error(`git init: unexpected trailing argument '${args[i + 1]}'`);
            }
            validateWorkDir(args[i]);
        } else if (cmdStart === 0) {
            // No --git-dir, not init: validate workDir
            validateWorkDir(workDir);
        }
        // If --git-dir was present and subcommand is not init, it was already validated above
    }
    if (options && options.trace) {
        process.stderr.write(`Called 'git ${args.join(" ")}' in '${workDir}':\n${new Error().stack}\n`);
    }

    return new Promise<string>((resolve, reject) => {
        if (options && options.lineHandler) {
            const lineHandler = options.lineHandler;
            if (options.processCallback) {
                reject(new Error("line handler *and* process callback set"));
                return;
            }
            options.processCallback = (process: ChildProcess): void => {
                process.on("exit", (code: number, signal: string) => {
                    if (signal) {
                        reject(new Error(`Received signal ${signal}`));
                    } else if (code) {
                        reject(new Error(`Received code ${code}`));
                    }
                });
                if (!process.stdout) {
                    throw new Error(`No stdout for "git ${args.join(" ")}`);
                }
                let linePromise: Promise<void> | undefined;
                const handleLine = (line: string): boolean => {
                    try {
                        if (!linePromise) {
                            linePromise = lineHandler(line);
                        } else {
                            linePromise = linePromise.then(() => {
                                return lineHandler(line);
                            });
                        }
                        linePromise.catch((reason) => {
                            reject(reason as Error);
                            process.kill();
                        });
                    } catch (reason) {
                        const error = reason as Error;
                        reject(error);
                        process.kill();
                        return false;
                    }
                    return true;
                };
                let buffer = "";
                process.stdout.on("data", (chunk: string) => {
                    buffer += chunk;
                    for (;;) {
                        const eol = buffer.indexOf("\n");
                        if (eol < 0) {
                            break;
                        }
                        if (!handleLine(buffer.substring(0, eol))) {
                            return;
                        }
                        buffer = buffer.substring(eol + 1);
                    }
                });
                process.stdout.on("end", () => {
                    if (buffer.length > 0) {
                        handleLine(buffer);
                    }
                    if (linePromise) {
                        linePromise
                            .then(() => {
                                resolve("");
                            })
                            .catch((reason) => {
                                reject(reason as Error);
                            });
                    } else {
                        resolve("");
                    }
                });
            };
        }

        GitProcess(args, workDir, options)
            .then((result) => {
                if (result.exitCode) {
                    reject(new Error(`git ${args.join(" ")} failed: ${result.exitCode},\n${result.stderr}`));
                    return;
                }
                if (options && options.trace) {
                    process.stderr.write(
                        `Output of 'git ${args.join(" ")}':\nstderr: ${result.stderr}\nstdout: ${result.stdout}\n`,
                    );
                }
                if (!options?.lineHandler) {
                    // let callback resolve the promise
                    resolve(
                        !options || options.trimTrailingNewline === false
                            ? result.stdout
                            : trimTrailingNewline(result.stdout),
                    );
                }
            })
            .catch((reason) => {
                reject(reason as Error);
            });
    });
}

/**
 * Call `git rev-parse --verify` to verify an object name.
 *
 * Note that it will *always* return the argument back to the user if it is
 * a hex string of length 40. This is consistent with `rev-parse`. To
 * verify objects by full SHA-1, you have to add `^{blob}` or similar.
 *
 * @param { string } argument the name referring to a Git object
 * @param { string | undefined } workDir
 *    the working directory in which to run `git rev-parse`
 * @returns { string | undefined } the full SHA-1, or undefined
 */
export async function revParse(argument: string, workDir?: string): Promise<string | undefined> {
    const dir = workDir || ".";
    validateWorkDir(dir);
    const result = await GitProcess(["rev-parse", "--verify", "-q", argument], dir);
    return result.exitCode ? undefined : trimTrailingNewline(result.stdout);
}

/**
 * Call `git rev-list --count` to count objects in a commit range.
 *
 * @param { string[] } rangeArgs the arguments to pass to `git rev-list`
 * @param { string | undefined } workDir
 *    the working directory in which to run `git rev-parse`
 * @returns number the number of commits in the commit range
 */
export async function revListCount(rangeArgs: string | string[], workDir = "."): Promise<number> {
    validateWorkDir(workDir);
    const gitArgs: string[] = ["rev-list", "--count"];
    if (typeof rangeArgs === "string") {
        gitArgs.push(rangeArgs);
    } else {
        gitArgs.push(...rangeArgs);
    }
    const result = await GitProcess(gitArgs, workDir);
    if (result.exitCode) {
        throw new Error(`Could not determine count for ${rangeArgs}: ${result.stderr}`);
    }
    return parseInt(result.stdout, 10);
}

/**
 * Determine whether a certain commit exists
 *
 * @param {string} commit the name of the commit
 * @param {string} workDir the Git worktree where to look
 * @returns {boolean} whether the commit exists
 */
export async function commitExists(commit: string, workDir: string): Promise<boolean> {
    return (await revParse(`${commit}^{commit}`, workDir)) !== undefined;
}

export async function gitConfig(key: string, workDir?: string): Promise<string | undefined> {
    const dir = workDir || ".";
    validateWorkDir(dir);
    const result = await GitProcess(["config", key], dir);
    if (result.exitCode !== 0) {
        return undefined;
    }
    return trimTrailingNewline(result.stdout);
}

export async function gitConfigForEach(
    key: string,
    callbackfn: (value: string) => void,
    workDir?: string,
): Promise<void> {
    const dir = workDir || ".";
    validateWorkDir(dir);
    const result = await GitProcess(["config", "--get-all", key], dir);
    result.stdout.split(/\r?\n/).map(callbackfn);
}

export async function gitCommandExists(command: string, workDir?: string): Promise<boolean> {
    const dir = workDir || ".";
    validateWorkDir(dir);
    const result = await GitProcess([command, "-h"], dir);
    return result.exitCode === 129;
}

// rev-parse does not have enough info in shallow repos to determine a safe short name
export function gitShortHash(longHash: string): string {
    return longHash.substring(0, 8);
}
