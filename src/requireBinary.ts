const PACKAGE_NAME = `@lydell/node-pty-${process.platform}-${process.arch}`;

const help = `
This can happen if you use the "--omit=optional" (or "--no-optional") npm flag.
The "optionalDependencies" package.json feature is used to install the correct
binary executable for your current platform. Remove that flag to use @lydell/node-pty.

This can also happen if the "node_modules" folder was copied between two operating systems
that need different binaries - including "virtual" operating systems like Docker and WSL.
If so, try installing with npm rather than copying "node_modules".
`.trim();

export function requireBinary<T>(file: string): T {
  try {
    return require(`${PACKAGE_NAME}/${file}`);
  } catch (error) {
    if (error && (error as NodeJS.ErrnoException).code === 'MODULE_NOT_FOUND') {
      const optionalDependencies = getOptionalDependencies();
      throw new Error(
        optionalDependencies === undefined
          ? `The @lydell/node-pty package could not find the binary package: ${PACKAGE_NAME}/${file}\n\n${help}\n\nYour platform (${process.platform}-${process.arch}) might not be supported.`
          : PACKAGE_NAME in optionalDependencies
            ? `The @lydell/node-pty package supports your platform (${process.platform}-${process.arch}), but it could not find the binary package for it: ${PACKAGE_NAME}/${file}\n\n${help}`
            : `The @lydell/node-pty package currently does not support your platform: ${process.platform}-${process.arch}`,
        // @ts-ignore: The TypeScript setup does not seem to support cause.
        {cause: error}
      );
    } else {
      throw error;
    }
  }
}

function getOptionalDependencies(): Record<string, string> | undefined {
  try {
    return require('./package.json').optionalDependencies;
  } catch (_error) {
    return undefined;
  }
}
