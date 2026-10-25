const frame = document.createElement("iframe");
frame.src = browser.runtime.getURL("page.html") + "?" + location.pathname.split("/").pop().replace(".html", "");
window.addEventListener("message", event => {
    if (event.data?.portTestOutput) {
        document.documentElement.dataset.portOutput = JSON.stringify(event.data.portTestOutput);
        document.dispatchEvent(new Event("port-finished"));
        frame.remove();
    }
});
document.body.append(frame);
