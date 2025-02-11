/**
 * Copyright (c) 2018, Microsoft Corporation (MIT License).
 */

interface IConptyNative {
  startProcess(file: string, cols: number, rows: number, debug: boolean, pipeName: string, conptyInheritCursor: boolean): IConptyProcess;
  connect(ptyId: number, commandLine: string, cwd: string, env: string[], onExitCallback: (exitCode: number) => void): { pid: number };
  resize(ptyId: number, cols: number, rows: number): void;
  clear(ptyId: number): void;
  kill(ptyId: number): void;
}

interface IUnixNative {
  fork(file: string, args: string[], parsedEnv: string[], cwd: string, cols: number, rows: number, uid: number, gid: number, useUtf8: boolean, helperPath: string, onExitCallback: (code: number, signal: number) => void): IUnixProcess;
  open(cols: number, rows: number): IUnixOpenProcess;
  process(fd: number, pty?: string): string;
  resize(fd: number, cols: number, rows: number): void;
}

interface IConptyProcess {
  pty: number;
  fd: number;
  conin: string;
  conout: string;
}

interface IUnixProcess {
  fd: number;
  pid: number;
  pty: string;
}

interface IUnixOpenProcess {
  master: number;
  slave: number;
  pty: string;
}
