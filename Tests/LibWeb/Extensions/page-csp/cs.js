// Requests each URL the page asks for, from the content script's world, with fetch(), XMLHttpRequest, WebSocket, or
// EventSource, and reports the result into the DOM.
function fetchResult(url) {
    return fetch(url).then(
        async response => ({ status: response.status, body: await response.text() }),
        error => ({ failed: error.name })
    );
}

function xhrResult(url) {
    return new Promise(resolve => {
        const xhr = new XMLHttpRequest();
        xhr.open("GET", url);
        xhr.onload = () => resolve({ status: xhr.status, body: xhr.responseText });
        xhr.onerror = () => resolve({ failed: "error" });
        xhr.send();
    });
}

function webSocketResult(url) {
    return new Promise(resolve => {
        const socket = new WebSocket(url);
        socket.onopen = () => {
            socket.close();
            resolve({ opened: true });
        };
        socket.onerror = () => resolve({ failed: "error" });
    });
}

function eventSourceResult(url) {
    return new Promise(resolve => {
        const source = new EventSource(url);
        source.onopen = () => {
            source.close();
            resolve({ opened: true });
        };
        source.onerror = () => {
            source.close();
            resolve({ failed: "error" });
        };
    });
}

const requesters = { fetch: fetchResult, xhr: xhrResult, websocket: webSocketResult, eventsource: eventSourceResult };

document.addEventListener("tabs-request", async event => {
    const message = JSON.parse(event.detail);
    const reply = await requesters[message.type](message.url);
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
