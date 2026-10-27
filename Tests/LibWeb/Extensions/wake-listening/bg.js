// Records the clicks on its button since it last started.
const clicks = [];
browser.action.onClicked.addListener(() => clicks.push("click"));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "clicks") sendResponse(clicks.splice(0));
});
