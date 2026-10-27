// The background page's frame is the one that listens to the button.
window.clicks = [];
browser.browserAction.onClicked.addListener(() => window.clicks.push("click"));
