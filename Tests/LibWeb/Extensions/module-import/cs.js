// Loads the extension's modules the way a bundled extension does: a dynamic import for the content script's own
// world, and a script element for the page's. The page's policy allows neither for scripts of its own.
const report = {};
const pageScript = document.createElement("script");
pageScript.src = browser.runtime.getURL("modules/page.js");
const pageScriptDone = new Promise(resolve => {
    pageScript.addEventListener("load", () => resolve("loaded"));
    pageScript.addEventListener("error", () => resolve("failed"));
});
document.head.append(pageScript);
(async () => {
    try {
        const first = await import(browser.runtime.getURL("modules/entry.js"));
        const second = await import(browser.runtime.getURL("modules/entry.js"));
        report.entry = first.describe();
        report.sameInstance = first === second;
    } catch (error) {
        report.error = `${error.name}: ${error.message}`;
    }
    report.pageScript = await pageScriptDone;
    document.documentElement.dataset.report = JSON.stringify(report);
})();
