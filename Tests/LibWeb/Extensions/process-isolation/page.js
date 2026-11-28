// Opens the web page it's given without an opener, and reports the process it runs in itself.
window.open(new URLSearchParams(location.search).get("target"), "_blank", "noopener");
browser.runtime.sendMessage({ type: "extension", pid: internals.webContentProcessId() });
