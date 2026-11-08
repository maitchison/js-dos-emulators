// Guest filesystem transfers, timestamped in the worker's clock domain.
{
  Module.furyDiskRunning = false;
  Module.furyDiskHostDepth = 0;
  Module.fsReads = 0;
  Module.fsReadBytes = 0;
  let pending = [];
  let scheduled = false;
  let sequence = 0;

  function flushDiskActivity() {
    if (!pending.length) return;
    const events = pending;
    pending = [];
    Module.sendMessage('ws-log', {
      tag: 'fury-disk-v1',
      message: JSON.stringify({ sequence: ++sequence, origin: performance.timeOrigin, events }),
    });
  }

  for (const operation of ['read', 'write']) {
    const original = Module.FS[operation];
    Module.FS[operation] = function(stream, ...args) {
      const path = stream?.path || '';
      const guest = Module.furyDiskRunning && !Module.furyDiskHostDepth &&
        Module.FS.isFile(stream?.node?.mode) && path.startsWith('/home/web_user/') &&
        !path.startsWith('/home/web_user/.jsdos/');
      const start = guest ? performance.now() : 0;
      const bytes = original.call(this, stream, ...args);
      if (guest && bytes > 0) {
        const end = performance.now();
        if (operation === 'read') {
          Module.fsReads++;
          Module.fsReadBytes += bytes;
          // Background music stays in the I/O totals but does not drive the case light or SFX.
          if (/\/res\/music\//i.test(path)) return bytes;
        }
        pending.push([start, end, operation, bytes]);
        // Bound message size without adding a polling delay to short bursts.
        if (pending.length === 64) flushDiskActivity();
        if (!scheduled) {
          scheduled = true;
          queueMicrotask(() => {
            scheduled = false;
            flushDiskActivity();
          });
        }
      }
      return bytes;
    };
  }
}
