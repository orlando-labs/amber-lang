import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

const AMBER_TASK_TYPE = "amber";

interface AmberTaskDefinition extends vscode.TaskDefinition {
  /** Which amberc action to perform. */
  command: "run" | "build";
  /** Path to the .am file. Defaults to the active editor's file. */
  file?: string;
  /** Extra arguments passed to amberc. */
  args?: string[];
}

/** Returns true if `p` exists and is a file. */
function isFile(p: string): boolean {
  try {
    return fs.statSync(p).isFile();
  } catch {
    return false;
  }
}

/** Returns true if a bare command name is resolvable on PATH. */
function isOnPath(name: string): boolean {
  const pathVar = process.env.PATH;
  if (!pathVar) {
    return false;
  }
  const exts = process.platform === "win32"
    ? (process.env.PATHEXT ?? ".EXE;.CMD;.BAT").split(";")
    : [""];
  for (const dir of pathVar.split(path.delimiter)) {
    if (!dir) {
      continue;
    }
    for (const ext of exts) {
      if (isFile(path.join(dir, name + ext))) {
        return true;
      }
    }
  }
  return false;
}

/**
 * Resolves the amberc executable to use.
 *
 * Order: the `amber.compilerPath` setting (if absolute or found on PATH), then a
 * fallback to `<workspaceFolder>/build/amberc` (the in-repo build output).
 * Returns `undefined` if nothing resolves.
 */
function resolveAmberc(resource?: vscode.Uri): string | undefined {
  const configured = vscode.workspace
    .getConfiguration("amber", resource)
    .get<string>("compilerPath", "amberc");

  if (path.isAbsolute(configured)) {
    return isFile(configured) ? configured : undefined;
  }

  // Relative path that contains a separator: resolve against the workspace folder.
  if (configured.includes(path.sep) || configured.includes("/")) {
    const folder = workspaceFolderFor(resource);
    if (folder) {
      const resolved = path.resolve(folder.uri.fsPath, configured);
      if (isFile(resolved)) {
        return resolved;
      }
    }
  } else if (isOnPath(configured)) {
    return configured;
  }

  // Fallback: in-repo build output.
  const folder = workspaceFolderFor(resource);
  if (folder) {
    const fallback = path.join(folder.uri.fsPath, "build", "amberc");
    if (isFile(fallback)) {
      return fallback;
    }
  }
  return undefined;
}

function workspaceFolderFor(resource?: vscode.Uri): vscode.WorkspaceFolder | undefined {
  if (resource) {
    const folder = vscode.workspace.getWorkspaceFolder(resource);
    if (folder) {
      return folder;
    }
  }
  return vscode.workspace.workspaceFolders?.[0];
}

/** Builds the amberc argv (excluding the executable) for a run/build definition. */
function ambercArgs(
  def: AmberTaskDefinition,
  file: string,
  resource?: vscode.Uri,
): string[] {
  const config = vscode.workspace.getConfiguration("amber", resource);
  if (def.command === "build") {
    const target = config.get<string>("build.target", "native");
    const outDir = config.get<string>("build.outDir", "build");
    const folder = workspaceFolderFor(resource);
    const baseDir = path.isAbsolute(outDir)
      ? outDir
      : path.resolve(folder ? folder.uri.fsPath : path.dirname(file), outDir);
    const stem = path.basename(file, path.extname(file));
    const out = path.join(baseDir, stem);
    return ["build", file, "-o", out, "--target", target, ...(def.args ?? [])];
  }
  const runArgs = config.get<string[]>("run.args", []);
  return [file, ...runArgs, ...(def.args ?? [])];
}

/** Constructs a vscode.Task for a run/build definition. */
function makeTask(
  def: AmberTaskDefinition,
  file: string,
  resource?: vscode.Uri,
): vscode.Task | undefined {
  const amberc = resolveAmberc(resource);
  if (!amberc) {
    return undefined;
  }
  const folder = workspaceFolderFor(resource);
  const scope: vscode.WorkspaceFolder | vscode.TaskScope =
    folder ?? vscode.TaskScope.Workspace;
  const name = `${def.command} ${path.basename(file)}`;
  const execution = new vscode.ShellExecution(amberc, ambercArgs(def, file, resource));
  const task = new vscode.Task(def, scope, name, AMBER_TASK_TYPE, execution, []);
  task.presentationOptions = {
    reveal: vscode.TaskRevealKind.Always,
    panel: vscode.TaskPanelKind.Shared,
    clear: true,
  };
  task.group =
    def.command === "build" ? vscode.TaskGroup.Build : undefined;
  return task;
}

/** The active editor's .am file, or undefined (with a user-facing message). */
async function activeAmberFile(): Promise<vscode.TextDocument | undefined> {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "amber") {
    void vscode.window.showErrorMessage("Amber: no active .am file.");
    return undefined;
  }
  if (editor.document.isUntitled) {
    void vscode.window.showErrorMessage("Amber: save the file before running.");
    return undefined;
  }
  await editor.document.save();
  return editor.document;
}

async function runCommand(command: "run" | "build"): Promise<void> {
  const doc = await activeAmberFile();
  if (!doc) {
    return;
  }
  const file = doc.uri.fsPath;
  const task = makeTask({ type: AMBER_TASK_TYPE, command }, file, doc.uri);
  if (!task) {
    void vscode.window.showErrorMessage(
      "Amber: could not find the amberc compiler. Set 'amber.compilerPath' or build it with 'make build'.",
    );
    return;
  }
  await vscode.tasks.executeTask(task);
}

const taskProvider: vscode.TaskProvider = {
  provideTasks(): vscode.Task[] {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== "amber" || editor.document.isUntitled) {
      return [];
    }
    const file = editor.document.uri.fsPath;
    const tasks: vscode.Task[] = [];
    for (const command of ["run", "build"] as const) {
      const task = makeTask({ type: AMBER_TASK_TYPE, command }, file, editor.document.uri);
      if (task) {
        tasks.push(task);
      }
    }
    return tasks;
  },
  resolveTask(task: vscode.Task): vscode.Task | undefined {
    const def = task.definition as AmberTaskDefinition;
    const file = def.file ?? vscode.window.activeTextEditor?.document.uri.fsPath;
    if (!file || (def.command !== "run" && def.command !== "build")) {
      return undefined;
    }
    const resource = vscode.Uri.file(file);
    // resolveTask must reuse the definition object from the input task.
    const amberc = resolveAmberc(resource);
    if (!amberc) {
      return undefined;
    }
    const folder = workspaceFolderFor(resource);
    const scope = folder ?? vscode.TaskScope.Workspace;
    const execution = new vscode.ShellExecution(amberc, ambercArgs(def, file, resource));
    return new vscode.Task(def, scope, task.name, AMBER_TASK_TYPE, execution, []);
  },
};

export function activate(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.commands.registerCommand("amber.runFile", () => runCommand("run")),
    vscode.commands.registerCommand("amber.buildFile", () => runCommand("build")),
    vscode.tasks.registerTaskProvider(AMBER_TASK_TYPE, taskProvider),
  );
}

export function deactivate(): void {
  // No resources to release.
}
