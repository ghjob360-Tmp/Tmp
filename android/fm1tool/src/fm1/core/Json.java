// SPDX-License-Identifier: MPL-2.0
// Just enough JSON for a logue unit's manifest.json: objects (Map), arrays (List), strings, numbers (Double / Long),
// true, false, null. Pure Java (the desktop tests have no org.json).
package fm1.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public final class Json {
    final String s;
    int i;

    Json(String s) { this.s = s; }

    public static Object parse(String text) throws Fm1Exception {
        Json j = new Json(text);
        Object v = j.value();
        j.ws();
        if (j.i != j.s.length()) throw j.bad();
        return v;
    }

    Fm1Exception bad() { return new Fm1Exception("json", "JSON 格式不對（第 " + i + " 字）"); }

    void ws() {
        while (i < s.length() && Character.isWhitespace(s.charAt(i))) i++;
    }

    Object value() throws Fm1Exception {
        ws();
        if (i >= s.length()) throw bad();
        char c = s.charAt(i);
        if (c == '{') {
            Map<String, Object> m = new LinkedHashMap<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == '}') { i++; return m; }
            for (;;) {
                ws();
                if (i >= s.length() || s.charAt(i) != '"') throw bad();
                String k = str();
                ws();
                if (i >= s.length() || s.charAt(i++) != ':') throw bad();
                m.put(k, value());
                ws();
                if (i >= s.length()) throw bad();
                char d = s.charAt(i++);
                if (d == '}') return m;
                if (d != ',') throw bad();
            }
        }
        if (c == '[') {
            List<Object> a = new ArrayList<>();
            i++;
            ws();
            if (i < s.length() && s.charAt(i) == ']') { i++; return a; }
            for (;;) {
                a.add(value());
                ws();
                if (i >= s.length()) throw bad();
                char d = s.charAt(i++);
                if (d == ']') return a;
                if (d != ',') throw bad();
            }
        }
        if (c == '"') return str();
        if (s.startsWith("true", i)) { i += 4; return Boolean.TRUE; }
        if (s.startsWith("false", i)) { i += 5; return Boolean.FALSE; }
        if (s.startsWith("null", i)) { i += 4; return null; }
        int st = i;
        while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) i++;
        if (st == i) throw bad();
        String n = s.substring(st, i);
        try {
            if (n.contains(".") || n.contains("e") || n.contains("E")) return Double.parseDouble(n);
            return Long.parseLong(n);
        } catch (NumberFormatException e) {
            throw bad();
        }
    }

    String str() throws Fm1Exception {
        StringBuilder b = new StringBuilder();
        i++;                                            /* the opening quote */
        while (i < s.length()) {
            char c = s.charAt(i++);
            if (c == '"') return b.toString();
            if (c == '\\') {
                if (i >= s.length()) throw bad();
                char e = s.charAt(i++);
                switch (e) {
                    case 'n': b.append('\n'); break;
                    case 't': b.append('\t'); break;
                    case 'r': b.append('\r'); break;
                    case 'b': b.append('\b'); break;
                    case 'f': b.append('\f'); break;
                    case 'u':
                        if (i + 4 > s.length()) throw bad();
                        b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                        i += 4;
                        break;
                    default: b.append(e);
                }
            } else {
                b.append(c);
            }
        }
        throw bad();
    }
}
