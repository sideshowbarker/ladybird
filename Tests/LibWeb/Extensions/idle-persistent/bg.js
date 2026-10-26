// Counts the messages it has answered, so a page that unloaded and started again starts counting from one.
let count = 0;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "count") sendResponse(++count);
});
browser.runtime.onConnect.addListener(port => port.onMessage.addListener(() => port.postMessage(++count)));
