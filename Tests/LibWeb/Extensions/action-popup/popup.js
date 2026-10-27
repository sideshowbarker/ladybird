// Sizes itself as its URL asks, reports to the background page with the tab whose button opened it (the active tab
// of its current window), and follows the commands meant for it.
const content = document.getElementById("content");
const parameters = new URLSearchParams(location.search);
content.style.width = `${parameters.get("w") ?? 100}px`;
content.style.height = `${parameters.get("h") ?? 50}px`;

let openerTab = null;
browser.tabs.query({ active: true, currentWindow: true }).then(tabs => {
    openerTab = tabs.length === 1 ? tabs[0].id : null;
    browser.runtime.sendMessage({ type: "popup-hello", openerTab });
});

browser.runtime.onMessage.addListener(message => {
    if (message.type !== "popup-command" || message.openerTab !== openerTab) return;
    if (message.command === "close") window.close();
    if (message.command === "push-state") {
        history.pushState(null, "", "#pushed");
        browser.runtime.sendMessage({ type: "popup-hello", openerTab });
    }
    if (message.command === "navigate") location.href = "popup.html?navigated";
    if (message.command === "shrink") {
        content.style.width = "60px";
        content.style.height = "40px";
    }
    if (message.command === "grow") {
        content.style.width = "300px";
        content.style.height = "200px";
    }
});
