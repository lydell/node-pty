const childProcess = require("child_process");
const fs = require("fs");
const path = require("path");

const REPO_ROOT = path.join(__dirname, "..");
const DIST = path.join(REPO_ROOT, "dist");
const ARTIFACTS = path.join(REPO_ROOT, "artifacts");

const [runId] = process.argv.slice(2);

if (runId === undefined) {
  console.error("You need to pass the GitHub Actions run ID as the first argument.");
  process.exit(1);
}

console.log(`
Make sure that:

1. You have updated "forkVersion", "basedOnVersion", and "basedOnCommit" in package.json.
2. You have pushed that to GitHub.
3. The GitHub Actions run with the ID ${runId} is for that push, and has finished.

Press any key to continue. (Build, test, download artifacts, and publish.)
`.trim());

const waitResult = childProcess.spawnSync("node", ["-e", "process.stdin.setRawMode(true); process.stdin.on('data', (data) => process.exit(data.toString() === '\\x03' ? 1 : 0)); process.stdin.resume();"], {
  stdio: "inherit",
});
if (waitResult.status !== 0) {
  process.exit(waitResult.status);
}

// Build and test.
const testResult = childProcess.spawnSync("npm", ["test"], {
  shell: true,
  stdio: "inherit",
});
if (testResult.status !== 0) {
  process.exit(testResult.status);
}

fs.rmSync(ARTIFACTS, { recursive: true, force: true });

console.info("Downloading artifacts");
const ghResult = childProcess.spawnSync("gh", ["run", "download", runId, "--dir", ARTIFACTS], {
  shell: true,
  stdio: "inherit",
});
if (ghResult.status !== 0) {
  process.exit(ghResult.status);
}

const artifactDirs = fs.readdirSync(ARTIFACTS).map((dir) => path.join(ARTIFACTS, dir));

for (const dir of artifactDirs) {
  for (const file of fs.readdirSync(dir)) {
    if (file.endsWith(".node") || file.endsWith(".pdb") || file === "spawn-helper") {
      // At least on macOS, the files need to be executable, but that’s lost when
      // downloading them from GitHub Actions:
      // https://github.com/actions/upload-artifact#permission-loss
      fs.chmodSync(
        path.join(dir, file),
        0o755 // executable
      );
    }
  }
}

for (const dir of [...artifactDirs, DIST]) {
  console.info();
  console.info(`@lydell/${dir === DIST ? "node-pty" : path.basename(dir)}`);

  const publishResult = childProcess.spawnSync("npm", ["publish", "--access=public"], {
    cwd: dir,
    shell: true,
    stdio: "inherit",
  });
  if (publishResult.status !== 0) {
    process.exit(publishResult.status);
  }
}
