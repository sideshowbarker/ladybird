// Reports the process the web page runs in, which the page's own script recorded.
browser.runtime.sendMessage({ type: "web", pid: Number(document.documentElement.dataset.pid) });
