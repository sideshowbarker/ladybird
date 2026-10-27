// Declares each kind of top-level binding, then reports how the script itself sees them. The functions report what
// their bodies see: the extension's namespace, the script's own var, and a global of the page's, which they mustn't.
function declared() {
    return [typeof browser, typeof declaredVar, typeof pageGlobal].join(", ");
}
async function asyncDeclared() {
    return [typeof browser, typeof declaredVar, typeof pageGlobal].join(", ");
}
var declaredVar = "var";
let declaredLet = "let";
class DeclaredClass {}

(async () => {
    const results = {};
    const probe = (name, expression) => {
        try {
            results[name] = String(expression());
        } catch (error) {
            results[name] = `${error.name}: ${error.message}`;
        }
    };
    probe("declared()", () => declared());
    probe("window.declared", () => typeof window.declared);
    probe("declaredVar", () => declaredVar);
    probe("window.declaredVar", () => window.declaredVar);
    probe("declaredLet", () => declaredLet);
    probe("new DeclaredClass", () => typeof new DeclaredClass());
    try {
        results["asyncDeclared()"] = await asyncDeclared();
    } catch (error) {
        results["asyncDeclared()"] = `${error.name}: ${error.message}`;
    }
    document.documentElement.dataset.declarations = JSON.stringify(results);
    document.dispatchEvent(new CustomEvent("declarations-reported"));
})();
