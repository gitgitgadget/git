import { createHash } from "crypto";
import { git, revParse } from "./git.js";
import { GitNotes } from "./git-notes.js";
import { IGitGitGadgetOptions } from "./gitgitgadget.js";
import { GitHubGlue } from "./github-glue.js";
import { IMailMetadata } from "./mail-metadata.js";
import { IPatchSeriesMetadata } from "./patch-series-metadata.js";
import { IConfig } from "./project-config.js";
import { getPullRequestKey } from "./pullRequestKey.js";
import { IParsedMBox, parseMBox, parseMBoxMessageIDAndReferences } from "./send-mail.js";
import { SousChef } from "./sous-chef.js";

export const stateKey = "git@vger.kernel.org <-> GitGitGadget";
const replyToThisURL = "https://gitgitgadget.github.io/reply-to-this";

export interface IGitMailingListMirrorState {
    latestRevision?: string;
}

export class MailArchiveGitHelper {
    public static async get(
        config: IConfig,
        gggNotes: GitNotes,
        mailArchiveGitDir: string,
        githubGlue: GitHubGlue,
        branch: string,
    ): Promise<MailArchiveGitHelper> {
        const state: IGitMailingListMirrorState = (await gggNotes.get<IGitMailingListMirrorState>(stateKey)) || {};
        return new MailArchiveGitHelper(config, gggNotes, mailArchiveGitDir, githubGlue, state, branch);
    }

    /**
     * Returns the object name Git would generate if the key (plus a trailing
     * newline) were fed to `git hash-object`.
     *
     * @param key the content to hash (a newline is automatically appended)
     * @returns the object name
     */
    public static hashKey(key: string): string {
        const hash = createHash("sha1", { encoding: "utf8" });
        hash.update(`blob ${Buffer.byteLength(key) + 1}`);
        hash.update(`\0${key}\n`);
        return hash.digest("hex");
    }

    public static mbox2markdown(mbox: IParsedMBox): string {
        const body = mbox.body;

        if (!body.length) {
            return "";
        }

        const backTicks = "``````````";
        const wrapTop = `${backTicks}email\n`;
        const wrapBottom = `${backTicks}\n`;
        return `${wrapTop}${body}${body.endsWith("\n") ? "" : "\n"}${wrapBottom}`;
    }

    protected readonly branch: string;
    protected readonly config: IConfig;
    protected readonly state: IGitMailingListMirrorState;
    protected readonly gggNotes: GitNotes;
    protected readonly mailArchiveGitDir: string;
    protected readonly githubGlue: GitHubGlue;

    protected constructor(
        config: IConfig,
        gggNotes: GitNotes,
        mailArchiveGitDir: string,
        githubGlue: GitHubGlue,
        state: IGitMailingListMirrorState,
        branch: string,
    ) {
        this.config = config;
        this.branch = branch;
        this.gggNotes = gggNotes;
        this.mailArchiveGitDir = mailArchiveGitDir;
        this.githubGlue = githubGlue;
        this.state = state;
    }

    public async processMails(prFilter?: (pullRequestURL: string) => boolean): Promise<boolean> {
        const keys: Set<string> = new Set<string>();
        (await git(["ls-tree", "-r", `${this.gggNotes.notesRef}:`], { workDir: this.gggNotes.workDir }))
            .split("\n")
            .map((line: string) => {
                keys.add(line.substring(53).replace(/\//g, ""));
            });
        const seen = (messageID: string): boolean => {
            return keys.has(MailArchiveGitHelper.hashKey(messageID));
        };

        const handleWhatsCooking = async (mbox: string): Promise<void> => {
            const options = await this.gggNotes.get<IGitGitGadgetOptions>("");
            if (!options || !options.openPRs) {
                return;
            }
            /*
             * This map points from branch names in `gitster/git` to their
             * corresponding Pull Request URL.
             */
            const branchNameMap = new Map<string, string>();
            for (const pullRequestURL of Object.keys(options.openPRs)) {
                if (prFilter && !prFilter(pullRequestURL)) {
                    continue;
                }
                const prMeta = await this.gggNotes.get<IPatchSeriesMetadata>(pullRequestURL);
                if (prMeta && prMeta.branchNameInGitsterGit) {
                    branchNameMap.set(prMeta.branchNameInGitsterGit, pullRequestURL);
                }
            }
            const sousChef = new SousChef(mbox);
            if (!sousChef.messageID) {
                throw new Error(`Could not parse Message-ID of ${mbox}`);
            }
            console.log(`Handling "${sousChef.subject}"`);
            const whatsCookingBaseURL = this.config.mailrepo.url;
            for (const branchName of sousChef.branches.keys()) {
                const pullRequestURL = branchNameMap.get(branchName);
                if (pullRequestURL) {
                    const branchBaseURL = "https://github.com/gitgitgadget/git/commits/";
                    const info = sousChef.branches.get(branchName);
                    const pre = info?.text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
                    let comment: string;
                    if (!pre || pre.trim() === "") {
                        comment = `The branch [\`${branchName}\`](${branchBaseURL}${
                            branchName
                        }) was mentioned in the "${info?.sectionName}" section of the [status updates](${
                            whatsCookingBaseURL
                        }${sousChef.messageID}) on the Git mailing list.`;
                    } else {
                        comment = `There was a [status update](${whatsCookingBaseURL}${sousChef.messageID}) in the "${
                            info?.sectionName
                        }" section about the branch [\`${branchName}\`](${branchBaseURL}${
                            branchName
                        }) on the Git mailing list:\n\n<pre>\n${pre}\n</pre>`;
                    }
                    console.log(`\n${pullRequestURL}: ${comment}`);
                    await this.githubGlue.addPRComment(pullRequestURL, comment);
                }
            }
        };

        const mboxHandler = async (mbox: string): Promise<void> => {
            const parsedMbox = await parseMBox(mbox, true);

            if (!parsedMbox.headers) {
                throw new Error(`Could not parse ${mbox}`);
            }
            const parsed = parseMBoxMessageIDAndReferences(parsedMbox);

            if (
                parsedMbox.subject?.match(/^What's cooking in git.git /) &&
                parsedMbox.from === "Junio C Hamano <gitster@pobox.com>"
            ) {
                return handleWhatsCooking(mbox);
            }

            if (seen(parsed.messageID)) {
                console.log(`Already handled: ${parsed.messageID}`);
                return;
            }

            let pullRequestURL: string | undefined;
            let originalCommit: string | undefined;
            let issueCommentId: number | undefined;
            let firstPatchLine: number | undefined;
            for (const reference of parsed.references.filter(seen)) {
                const data = await this.gggNotes.get<IMailMetadata>(reference);
                if (data && data.pullRequestURL) {
                    if (prFilter && !prFilter(data.pullRequestURL)) {
                        continue;
                    }
                    /* Cover letters were recorded with their tip commits */
                    const commit = reference.match(/^pull/) ? undefined : data.originalCommit;
                    if (!pullRequestURL || (!originalCommit && commit) || (!issueCommentId && data.issueCommentId)) {
                        pullRequestURL = data.pullRequestURL;
                        issueCommentId = data.issueCommentId;
                        firstPatchLine = data.firstPatchLine;
                        originalCommit = commit;
                    }
                }
            }
            if (!pullRequestURL) {
                return;
            }
            console.log(
                `Message-ID ${parsed.messageID} (length ${mbox.length}) for PR ${pullRequestURL}, commit ${
                    originalCommit
                }, comment ID: ${issueCommentId}`,
            );

            const archiveURL = `${this.config.mailrepo.url}${parsed.messageID}`;
            const authorName = parsedMbox.from ? parsedMbox.from.replace(/ *<.*>/, "") : "Somebody";
            const header =
                `**${authorName}** wrote [on the Git mailing list](${archiveURL}) ` +
                `([how to reply to this email](${replyToThisURL})):\n\n`;
            const comment = MailArchiveGitHelper.mbox2markdown(parsedMbox);
            const fullComment = header + comment;
            const prKey = getPullRequestKey(pullRequestURL);

            if (issueCommentId) {
                await this.githubGlue.addPRCommentReply(pullRequestURL, issueCommentId, fullComment);
            } else if (originalCommit) {
                try {
                    const result = await this.githubGlue.addPRCommitComment(
                        pullRequestURL,
                        originalCommit,
                        this.gggNotes.workDir,
                        fullComment,
                        firstPatchLine,
                    );
                    issueCommentId = result.id;
                } catch (_error) {
                    const commits = await this.githubGlue.getPRCommits(prKey.owner, prKey.pull_number);
                    const regarding = `${header.slice(0, -3)} regarding ${originalCommit}${
                        commits.find((e) => e.commit === originalCommit) ? "" : " (outdated)"
                    }:\n\n`;
                    await this.githubGlue.addPRComment(pullRequestURL, regarding + comment);
                    originalCommit = undefined;
                }
            } else {
                /*
                 * We will not use the ID of this comment, as it is an
                 * issue comment, really, not a Pull Request comment.
                 */
                await this.githubGlue.addPRComment(pullRequestURL, fullComment);
            }

            await this.githubGlue.addPRCc(pullRequestURL, parsedMbox.from || "");

            await this.gggNotes.set(parsed.messageID, {
                issueCommentId,
                messageID: parsed.messageID,
                originalCommit,
                pullRequestURL,
            });

            /* It is now known */
            keys.add(MailArchiveGitHelper.hashKey(parsed.messageID));
        };

        let buffer = "";
        let counter = 0;
        const lineHandler = async (line: string): Promise<void> => {
            if (line.startsWith("@@ ")) {
                // eslint-disable-next-line security/detect-unsafe-regex
                const match = line.match(/^@@ -(\d+,)?\d+ \+(\d+,)?(\d+)?/);
                if (match) {
                    if (counter) {
                        console.log(`Oops: unprocessed buffer ${buffer}`);
                    }
                    counter = parseInt(match[3], 10);
                    buffer = "";
                }
            } else if (counter && line.match(/^[ +]/)) {
                buffer += line.substring(1) + "\n";
                if (--counter) {
                    return;
                }
                try {
                    await mboxHandler(buffer);
                } catch (reason) {
                    console.log(`${reason}: skipping`);
                }
            }
        };

        if (!this.state.latestRevision) {
            throw new Error(
                [
                    "Mail archive email commit tip not set.  ",
                    "Please run `misc-helper init-email-commit-tip` to set the value.",
                ].join(""),
            );
        }

        const head = await revParse(this.branch, this.mailArchiveGitDir);
        if (this.state.latestRevision === head) {
            return false;
        }

        const range = `${this.state.latestRevision}..${head}`;
        console.log(`Handling commit range ${range}`);
        await git(["log", "-p", "-U99999", "--reverse", range], { lineHandler, workDir: this.mailArchiveGitDir });

        this.state.latestRevision = head;
        await this.gggNotes.set(stateKey, this.state, true);

        return true;
    }
}
