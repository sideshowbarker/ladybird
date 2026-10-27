// What the background injects into tabs that were open before it started: a mark on each frame, and its URL's file.
document.documentElement.dataset.injected = "yes";
location.pathname.split("/").pop() || location.href;
