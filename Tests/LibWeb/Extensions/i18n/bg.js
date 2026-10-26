browser.runtime.onMessage.addListener(type => {
    const output = [];
    const check = (name, value) => output.push(`${name}: ${value}`);
    try {
        const get = browser.i18n.getMessage;
        if (type === "i18n-get-message") {
            check("case and named placeholder", get("HELLO", "World") === "Hello World");
            check("nine positions", get("positional", [1, 2, 3, 4, 5, 6, 7, 8, 9]) === "1|2|3|4|5|6|7|8|9");
            check("invalid dollar", get("invalid") === "ab");
            check("escaped dollar", get("cost") === "cost $5");
            check("dollar run", get("run", ["x"]) === "$$1");
            check("single digit", get("ten", ["x"]) === "x0");
            check("missing position", get("second", ["x"]) === "");
            check("trailing dollar", get("trailing") === "end");
            check("no substitutions", get("ten") === "0");
            check("over nine", get("hello", Array(10).fill("x")) === undefined);
            check("missing message", get("missing") === "");
            check("escape before substitutions", get("escape", "<value>", { escapeLt: true }) === "&lt;<value>>");
            check("escape before dollar parsing", get("escape_dollar", [], { escapeLt: true }) === "lt;");
            check("escape requires true", get("escape", "x", { escapeLt: 1 }) === "<x>");
            check("array proxy ignored", get("hello", new Proxy(["x"], {})) === "Hello ");
            check(
                "object ignored",
                get("hello", {
                    toString() {
                        throw new Error("unused");
                    },
                }) === "Hello "
            );
            check(
                "throwing conversion skipped",
                get("positional", [
                    {
                        toString() {
                            throw new Error("skip");
                        },
                    },
                    "x",
                    "y",
                ]) === "x|y|||||||"
            );
            const throwsTypeError = call => {
                try {
                    call();
                    return false;
                } catch (error) {
                    return error instanceof TypeError;
                }
            };
            check(
                "name must be a string",
                throwsTypeError(() =>
                    get(
                        {
                            toString() {
                                return "hello";
                            },
                        },
                        "World"
                    )
                )
            );
            check(
                "number name rejected",
                throwsTypeError(() => get(1))
            );
            check(
                "missing name rejected",
                throwsTypeError(() => get())
            );
            check(
                "options must be an object",
                throwsTypeError(() => get("hello", "World", 5))
            );
            check("null options allowed", get("hello", "World", null) === "Hello World");
            const throwingElement = ["x"];
            Object.defineProperty(throwingElement, 0, {
                get() {
                    throw new Error("get");
                },
            });
            check("element getter failure", get("hello", throwingElement) === undefined);
            check(
                "option getter failure",
                get("escape", "x", {
                    get escapeLt() {
                        throw new Error("get");
                    },
                }) === "<x>"
            );
        } else if (type === "i18n-ui-language") {
            const language = browser.i18n.getUILanguage();
            check("UI language is nonempty", typeof language === "string" && language.length > 0);
            check("locale forms agree", language.replaceAll("-", "_") === get("@@ui_locale"));
            check("BCP 47 separator", !language.includes("_"));
        } else if (type === "i18n-manifest") {
            const manifest = browser.runtime.getManifest();
            check("localized name", manifest.name === "Localized extension");
            check("localized description", manifest.description === "Localized description");
            check("no current_locale", !("current_locale" in manifest));
        }
    } catch (error) {
        output.push(`ERROR: ${error.name}: ${error.message}`);
    }
    return Promise.resolve(output);
});
