// Records in storage.local, as it unloads, how many messages it answered, so the next page to start can report it. Its
// frame records that it was suspended too.
let count = 0;
const frame = document.createElement("iframe");
const frameLoaded = new Promise(resolve => frame.addEventListener("load", resolve));
frame.src = "frame.html";
document.body.appendChild(frame);
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "count") sendResponse(++count);
    else if (message.type === "frame-loaded") {
        frameLoaded.then(() => sendResponse(true));
        return true;
    } else if (message.type === "read") {
        browser.storage.local.get(["suspendedAfter", "frameSuspended"]).then(items => sendResponse(items));
        return true;
    }
});
browser.runtime.onSuspend.addListener(() => browser.storage.local.set({ suspendedAfter: count }));
