// Relays the page's requests for this extension's background page, under event names of its own.
document.addEventListener("host-only-request", async event => {
    const reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
    document.documentElement.dataset.hostOnlyReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("host-only-reply"));
});
document.documentElement.dataset.hostOnlyReady = "1";
document.dispatchEvent(new CustomEvent("host-only-ready"));
