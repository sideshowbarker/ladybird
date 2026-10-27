// Reports the child window's steps to the background page: its first load, its same-document navigations, and its
// load after navigating to another document. The run parameter keeps one test's window apart from another's.
const parameters = new URLSearchParams(location.search);
const run = parameters.get("run");
const report = step => browser.runtime.sendMessage({ type: "child", step, run });

if (parameters.has("second")) {
    report("afterNavigation");
} else {
    (async () => {
        await report("loaded");
        location.hash = "fragment";
        history.pushState(null, "", `?first-pushed&run=${run}`);
        history.replaceState(null, "", `?first-replaced&run=${run}`);
        await report("afterSameDocument");
        location.href = `${location.pathname}?second&run=${run}`;
    })();
}
