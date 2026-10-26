window.addEventListener("load", () => {
    const target = document.getElementById("target");
    const style = getComputedStyle(target);
    const output = [
        `linked CSS after unknown token: ${style.color === "rgb(12, 34, 56)"}`,
        `imported CSS: ${style.height === "37px"}`,
        `single substitution: ${style.getPropertyValue("--literal").trim() === '"__MSG_color__"'}`,
        `HTML stays literal: ${target.textContent === "__MSG_title__"}`,
        `extension page API: ${browser.i18n?.getMessage("title") === "Localized extension"}`,
    ];
    parent.postMessage({ i18nOutput: output }, "*");
});
