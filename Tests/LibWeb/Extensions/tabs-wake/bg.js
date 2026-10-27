// Records the titles that tabs change to while it runs, so a test can tell that a change woke it.
const titles = [];
browser.tabs.onUpdated.addListener((tabId, changeInfo) => {
    if (changeInfo.title) titles.push(changeInfo.title);
});
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "titles") sendResponse(titles);
});
