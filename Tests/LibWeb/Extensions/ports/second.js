browser.runtime.onConnect.addListener(port => {
    port.onMessage.addListener(message => {
        if (message === "first-left") {
            port.postMessage("second-alive");
        } else if (message === "close-second") {
            port.disconnect();
        }
    });
    port.postMessage("second-ready");
});
parent.postMessage("second-loaded", "*");
