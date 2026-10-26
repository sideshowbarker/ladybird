// Adds an element to the body as soon as the script runs, as background scripts commonly do.
let state;
try {
    const element = document.createElement("div");
    document.body.appendChild(element);
    state = `appended to ${element.parentNode.localName}`;
} catch (error) {
    state = `${error.name}: ${error.message}`;
}
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    sendResponse(state);
});
