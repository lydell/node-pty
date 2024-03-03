const fs = require("fs");
const path = require("path");
const originalPackageJson = require("../package.json");

const PACKAGE_NAME = `@lydell/node-pty-${process.platform}-${process.arch}`;
const REPO_ROOT = path.join(__dirname, "..");
const RELEASE = path.join(REPO_ROOT, "build/Release");
const DIST = path.join(REPO_ROOT, "node_modules", PACKAGE_NAME);

fs.rmSync(DIST, { recursive: true, force: true });
fs.mkdirSync(DIST, { recursive: true });

const packageJson = {
  "name": PACKAGE_NAME,
  "version": originalPackageJson.forkVersion,
  "description": `Prebuilt ${process.platform}-${process.arch} binaries for node-pty.`,
  "repository": {
    "type": "git",
    "url": "git://github.com/lydell/node-pty.git"
  },
  "license": originalPackageJson.license,
  "os": [ process.platform ],
  "cpu": [ process.arch ]
};

const readme = `
# ${packageJson.name}

${packageJson.description}
`.trim();

fs.writeFileSync(
    path.join(DIST, "package.json"),
    JSON.stringify(packageJson, null, 2)
);

fs.writeFileSync(
    path.join(DIST, "README.md"),
    readme
);

for (const file of fs.readdirSync(RELEASE)) {
    fs.copyFileSync(
        path.join(RELEASE, file),
        path.join(DIST, file)
    );
}

console.info(PACKAGE_NAME);
console.info(fs.readdirSync(DIST));
