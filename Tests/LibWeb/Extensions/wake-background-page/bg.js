// Answers for the clicks its frame heard since the page last started.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "clicks") sendResponse(window.frames[0].clicks.splice(0));
});
