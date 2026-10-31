// Registers a script that lasts as long as this page does, and tells the background page once it has.
browser.contentScripts
    .register({
        matches: ["file:///*/data/content-scripts-register-target.html?page"],
        js: [{ code: "document.documentElement.dataset.registerPage = 'p';" }],
        allFrames: true,
        runAt: "document_start",
    })
    .then(() => browser.runtime.sendMessage({ type: "page-registered" }));
