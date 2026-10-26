const type = location.pathname.split("/").pop().replace(".html", "");
function finish(output) {
    document.documentElement.dataset.i18nOutput = JSON.stringify(output);
    document.dispatchEvent(new Event("i18n-finished"));
}
if (type === "i18n-css-served") {
    const frame = document.createElement("iframe");
    frame.src = browser.runtime.getURL("page.html");
    window.addEventListener("message", event => {
        if (event.source === frame.contentWindow && event.data?.i18nOutput) {
            finish(event.data.i18nOutput);
            frame.remove();
        }
    });
    document.body.append(frame);
} else {
    browser.runtime.sendMessage(type).then(finish, error => finish([`ERROR: ${error}`]));
}
