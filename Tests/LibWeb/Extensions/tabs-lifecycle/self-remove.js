// Removes the tab this page is in.
browser.runtime.sendMessage({ type: "my-tab" }).then(id => browser.tabs.remove(id));
