// A second context in the background page, whose runtime.onSuspend listener records something of its own.
browser.runtime.onSuspend.addListener(() => browser.storage.local.set({ frameSuspended: true }));
