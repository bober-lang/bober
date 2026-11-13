// Starts build/bober lsp in the workspace's root, the bober repo, where its paths are relative to.
// It runs the last build/bober built: rebuild it with build/bober run -release src/bober.bob help after editing the compiler.
const { workspace } = require("vscode")
const { LanguageClient } = require("vscode-languageclient/node")

let client

exports.activate = () => {
	const folder = workspace.workspaceFolders?.[0]
	if (!folder) {
		return
	}
	const root = folder.uri.fsPath
	const server = { command: `${root}/build/bober`, args: ["lsp"], options: { cwd: root } }
	client = new LanguageClient("bober", "bober", server, { documentSelector: [{ language: "bober" }] })
	client.start()
}

exports.deactivate = () => client?.stop()
