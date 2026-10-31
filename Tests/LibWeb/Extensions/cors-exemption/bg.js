// Fetches what the test page asks for, with fetch() or XMLHttpRequest.
function xhrResult(url) {
    return new Promise(resolve => {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", url);
        xhr.onload = () =>
            resolve({ status: xhr.status, body: xhr.responseText, custom: xhr.getResponseHeader("X-Custom") });
        xhr.onerror = () => resolve({ failed: "error" });
        xhr.send();
    });
}

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(sendResponse);
        return true;
    };
    if (message.type === "fetch") return reply(fetchResult(message.url, message.init));
    if (message.type === "xhr") return reply(xhrResult(message.url));
});
