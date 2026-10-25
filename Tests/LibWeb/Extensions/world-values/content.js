const cases = [
    [CustomEvent, "detail"],
    [ErrorEvent, "error"],
    [MessageEvent, "data"],
    [PopStateEvent, "state"],
];
for (const [Constructor, property] of cases) {
    document.addEventListener(`page-${property}`, event => {
        const value = event[property];
        const result =
            value === null
                ? "null"
                : `${value.answer}/${Object.getPrototypeOf(value) === Object.prototype}/${value.constructor.constructor("return typeof browser")()}`;
        document.documentElement.setAttribute(`data-${property}`, result);
    });
}
document.addEventListener("run-extension-values", () => {
    for (const [Constructor, property] of cases) {
        const value = { answer: 42 };
        const ownEvent = new Constructor("own", { [property]: value });
        document.documentElement.setAttribute(`data-own-${property}`, ownEvent[property] === value);
        document.dispatchEvent(new Constructor(`extension-${property}`, { [property]: value }));
        document.dispatchEvent(new Constructor(`extension-${property}`, { [property]: () => browser }));
    }
    history.replaceState({ answer: 42 }, "");
});
document.addEventListener("read-page-history", () => {
    const value = history.state;
    document.documentElement.dataset.historyIdentity = history.state === value;
    document.documentElement.dataset.history = `${value.answer}/${Object.getPrototypeOf(value) === Object.prototype}/${value.constructor.constructor("return typeof browser")()}`;
});
window.addEventListener("error", event => {
    document.documentElement.dataset.ownError = event.error.message;
    event.preventDefault();
});
document.addEventListener("throw-extension-error", () => {
    throw new Error("private exception");
});
throw new Error("initial private exception");
