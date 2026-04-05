import fs from "node:fs";
import path from "node:path";

const META_PACKAGE_JSON = JSON.parse(
  fs.readFileSync(path.join(import.meta.dirname, "package.json"), "utf-8"),
);

const VERSION = META_PACKAGE_JSON.version;

const ORIGINAL_PACKAGE = path.join(
  import.meta.dirname,
  "node_modules",
  "node-pty",
);

const PACKAGES = path.join(import.meta.dirname, "packages");

const VARIANTS = fs
  .readdirSync(path.join(ORIGINAL_PACKAGE, "prebuilds"))
  .map((name) => {
    const [platform, arch] = name.split("-");
    return { platform, arch };
  });

function buildTopPackage() {
  const newPackage = path.join(PACKAGES, "top");

  const packageJson = {
    name: "@lydell/node-pty",
    description: "Smaller distribution of node-pty.",
    author: "Simon Lydell",
    version: VERSION,
    license: "MIT",
    type: "commonjs",
    exports: {
      ".": {
        types: "./node-pty.d.ts",
        default: "./index.js",
      },
    },
    types: "./node-pty.d.ts",
    repository: {
      type: "git",
      url: "git://github.com/lydell/node-pty.git",
    },
    keywords: [
      "pty",
      "tty",
      "terminal",
      "pseudoterminal",
      "forkpty",
      "openpty",
      "prebuild",
      "prebuilt",
    ],
    optionalDependencies: Object.fromEntries(
      VARIANTS.map(({ platform, arch }) => [
        `@lydell/node-pty-${platform}-${arch}`,
        VERSION,
      ]),
    ),
  };

  const readme = fs.readFileSync(
    path.join(import.meta.dirname, "README.md"),
    "utf8",
  );
  const newReadme = `
${readme.trim()}

## Version

@lydell/node-pty@${VERSION} is based on node-pty@${
    META_PACKAGE_JSON.dependencies["node-pty"]
  }.

## Prebuilt binaries

This package includes prebuilt binaries for the following platforms and architectures:

${VARIANTS.map(
  ({ platform, arch }) =>
    `- ${osName(platform)} ${archName(arch)} (${platform}-${arch})`,
).join("\n")}
`.trim();

  const types = fs
    .readFileSync(
      path.join(ORIGINAL_PACKAGE, "typings", "node-pty.d.ts"),
      "utf-8",
    )
    .replace(`declare module 'node-pty'`, `declare module '@lydell/node-pty'`);

  fs.mkdirSync(newPackage);

  fs.writeFileSync(
    path.join(newPackage, "package.json"),
    JSON.stringify(packageJson, null, 2),
  );

  fs.writeFileSync(path.join(newPackage, "README.md"), newReadme);

  fs.writeFileSync(path.join(newPackage, "node-pty.d.ts"), types);

  fs.cpSync(
    path.join(import.meta.dirname, "index.js"),
    path.join(newPackage, "index.js"),
  );
  verifyExports(
    path.join(import.meta.dirname, "index.js"),
    path.join(ORIGINAL_PACKAGE, "lib", "index.js"),
  );

  fs.cpSync(
    path.join(import.meta.dirname, "LICENSE"),
    path.join(newPackage, "LICENSE"),
  );
}

function osName(platform) {
  switch (platform) {
    case "darwin":
      return "macOS";
    case "linux":
      return "Linux";
    case "win32":
      return "Windows";
    default:
      throw new Error(`Unknown platform: ${platform}`);
  }
}

function archName(arch) {
  switch (arch) {
    case "x64":
      return "x86_64";
    case "arm64":
      return "ARM64";
    default:
      throw new Error(`Unknown arch: ${arch}`);
  }
}

function verifyExports(originalPath, newPath) {
  const regex = /^exports\..+void 0;$/m;
  const originalCode = fs.readFileSync(originalPath, "utf-8");
  const newCode = fs.readFileSync(originalPath, "utf-8");
  const originalMatch = regex.exec(originalCode);
  const newMatch = regex.exec(newCode);
  const ok =
    originalMatch !== null &&
    newMatch !== null &&
    originalMatch[0] === newMatch[0];
  if (!ok) {
    throw new Error(
      `
exports don’t match!

Original:
${originalMatch?.[0]}

New:
${newMatch?.[0]}
      `.trim(),
    );
  }
}

function buildSubPackage({ platform, arch }) {
  const name = `${platform}-${arch}`;
  const platformType = platform === "win32" ? "windows" : "unix";
  const newPackage = path.join(PACKAGES, name);

  fs.cpSync(ORIGINAL_PACKAGE, newPackage, {
    recursive: true,
    filter: (source) => {
      const segments = path.relative(ORIGINAL_PACKAGE, source).split(path.sep);
      const firstSegment = segments.at(0);
      const secondSegment = segments.at(1);
      const lastSegment = segments.at(-1);
      switch (firstSegment) {
        case "":
        case "LICENSE":
          return true;

        case "prebuilds":
          return secondSegment === undefined || secondSegment === name;

        case "lib":
          const type =
            lastSegment.includes("windows") ||
            lastSegment.includes("conpty") ||
            lastSegment.includes("conout")
              ? "windows"
              : lastSegment.includes("unix")
                ? "unix"
                : "shared";
          return (
            !lastSegment.includes(".") ||
            type === "shared" ||
            type === platformType
          );

        case "binding.gyp":
        case "package.json":
        case "README.md":
        case "scripts":
        case "src":
        case "third_party":
        case "typings":
          return false;

        default:
          throw new Error(
            `Need to decide if should be included: ${firstSegment}`,
          );
      }
    },
  });

  fix(newPackage);

  const packageJson = {
    name: `@lydell/node-pty-${platform}-${arch}`,
    version: VERSION,
    description: `The node-pty package, stripped down only for ${platform}-${arch}.`,
    repository: {
      type: "git",
      url: "git://github.com/lydell/node-pty.git",
    },
    license: "MIT",
    type: "commonjs",
    exports: "./lib/index.js",
    os: [platform],
    cpu: [arch],
  };

  const readme = `
# ${packageJson.name}

${packageJson.description}
`.trim();

  fs.writeFileSync(
    path.join(newPackage, "package.json"),
    JSON.stringify(packageJson, null, 2),
  );

  fs.writeFileSync(path.join(newPackage, "README.md"), readme);
}

// Remove empty directories.
function fix(dir) {
  const entries = fs.readdirSync(dir, { withFileTypes: true });
  let numEntries = entries.length;
  for (const entry of entries) {
    const entryPath = path.join(dir, entry.name);
    if (entry.isFile()) {
      // Do nothing.
    } else if (entry.isDirectory()) {
      const numChildEntries = fix(entryPath);
      if (numChildEntries === 0) {
        fs.rmdirSync(entryPath);
        numEntries--;
      }
    } else {
      throw new Error(`Neither a file nor directory: ${entryPath}`);
    }
  }
  return numEntries;
}

function run() {
  fs.rmSync(PACKAGES, { recursive: true, force: true });
  fs.mkdirSync(PACKAGES);

  buildTopPackage();

  for (const variant of VARIANTS) {
    buildSubPackage(variant);
  }
}

run();
