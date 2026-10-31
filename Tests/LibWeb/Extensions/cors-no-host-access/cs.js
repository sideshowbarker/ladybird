// Relays the test page's request to the background page, with events of this fixture's own.
document.addEventListener("no-host-request", async event => {
    const reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
    document.documentElement.dataset.noHostReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("no-host-reply"));
});
