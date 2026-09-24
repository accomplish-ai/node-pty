/**
 * Copyright (c) 2019, Microsoft Corporation (MIT License).
 *
 * This module fetches the console process list for a particular PID. It must be
 * called from a different process (child_process.fork) as there can only be a
 * single console attached to a process.
 */

import { loadNativeModule } from './utils';

const getConsoleProcessList = loadNativeModule('conpty_console_list').module.getConsoleProcessList;
const shellPid = parseInt(process.argv[2], 10);
let consoleProcessList: number[];
try {
  consoleProcessList = getConsoleProcessList(shellPid);
} catch (error) {
  // Natural exit can win the race with enumeration. Only ignore the error if
  // the shell no longer exists; keep unexpected helper failures visible.
  try {
    process.kill(shellPid, 0);
  } catch (probeError) {
    if (probeError.code === 'ESRCH') {
      process.send!({ consoleProcessList: [] });
      process.exit(0);
    }
  }
  throw error;
}
process.send!({ consoleProcessList });
process.exit(0);
