// SPDX-License-Identifier: MPL-2.0
// A logue SDK unit for the NTS-1 as CHAIN-1 stores it: an "LGU1" image (CHAIN-1's app/logue/logue.h), exactly as
// chain1/tools/logue/logue_pack.py makes it.
//  - mk1 (.ntkdigunit, a zip): its payload.bin as it is, its manifest's name, module and parameters in the header;
//  - mkII (.nts1mkiiunit, an ELF shared object): its loadable segments laid out from address 0 (the dynamic section is
//    inside them; CHAIN-1 links the unit itself). Its name and module are read from its unit_header.
package fm1.core;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class LogueUnit {
    public static final int HDR = 256;
    public static final String[] MODULE = {"", "MOD", "DELAY", "REVERB", "OSC"};

    public final byte[] image;          /* the LGU1 image to send */
    public final String name;
    public final int module, fmt;       /* module: 1 mod, 2 delay, 3 reverb, 4 osc; fmt: 1 mk1, 2 mkII */
    public final List<String> params;

    LogueUnit(byte[] image, String name, int module, int fmt, List<String> params) {
        this.image = image;
        this.name = name;
        this.module = module;
        this.fmt = fmt;
        this.params = params;
    }

    public String describe() {
        return name + " · " + (module >= 1 && module <= 4 ? MODULE[module] : "?") + " · " + (fmt == 2 ? "NTS-1 mkII" : "NTS-1");
    }

    static int u32(byte[] b, int o) { return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8 | (b[o + 2] & 0xFF) << 16 | (b[o + 3] & 0xFF) << 24; }
    static int u16(byte[] b, int o) { return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8; }
    static void put32(byte[] b, int o, long v) {
        for (int i = 0; i < 4; i++) b[o + i] = (byte) (v >>> (8 * i));
    }

    static long crc32(byte[] d) {
        CRC32 c = new CRC32();
        c.update(d, 0, d.length);
        return c.getValue();
    }

    static byte[] header(int fmt, int module, int size, int memsz, int dyn, long crc, String name, List<Object[]> params) {
        byte[] h = new byte[HDR];
        h[0] = 'L'; h[1] = 'G'; h[2] = 'U'; h[3] = '1';
        h[4] = (byte) fmt;
        h[5] = (byte) module;
        h[6] = (byte) params.size();
        put32(h, 8, size);
        put32(h, 12, memsz);
        put32(h, 16, dyn);
        put32(h, 20, crc);
        ascii(h, 24, name, 19);
        for (int i = 0; i < params.size() && i < 6; i++) {
            Object[] p = params.get(i);
            int o = 64 + 32 * i;
            ascii(h, o, (String) p[0], 19);
            int min = (int) p[1], max = (int) p[2];
            h[o + 20] = (byte) min; h[o + 21] = (byte) (min >> 8);
            h[o + 22] = (byte) max; h[o + 23] = (byte) (max >> 8);
            h[o + 24] = (byte) ("%".equals(p[3]) ? 1 : 0);
        }
        return h;
    }

    static void ascii(byte[] h, int o, String s, int max) {
        byte[] b = s.getBytes(StandardCharsets.US_ASCII);
        for (int i = 0; i < b.length && i < max; i++) h[o + i] = b[i] >= 32 && b[i] < 127 ? b[i] : (byte) '?';
    }

    static byte[] concat(byte[] a, byte[] b) {
        byte[] o = new byte[a.length + b.length];
        System.arraycopy(a, 0, o, 0, a.length);
        System.arraycopy(b, 0, o, a.length, b.length);
        return o;
    }

    /* ------------------------------------------------------------------------------------------- mk1 --- */
    @SuppressWarnings("unchecked")
    static LogueUnit mk1(byte[] payload, String manifestJson) throws Fm1Exception {
        Object root = Json.parse(manifestJson);
        if (!(root instanceof Map) || !(((Map<String, Object>) root).get("header") instanceof Map))
            throw new Fm1Exception("unit", "manifest.json 讀不懂");
        Map<String, Object> hd = (Map<String, Object>) ((Map<String, Object>) root).get("header");
        if (!"nutekt-digital".equals(hd.get("platform")))
            throw new Fm1Exception("unit", "這個單元是給 " + hd.get("platform") + " 的，不是 NTS-1");
        String mod = String.valueOf(hd.get("module"));
        int module = "modfx".equals(mod) ? 1 : "delfx".equals(mod) ? 2 : "revfx".equals(mod) ? 3 : "osc".equals(mod) ? 4 : 0;
        if (module == 0) throw new Fm1Exception("unit", "不支援的單元類型：" + mod);
        List<Object[]> params = new ArrayList<>();
        List<String> names = new ArrayList<>();
        Object ps = hd.get("params");
        if (ps instanceof List)
            for (Object o : (List<Object>) ps) {
                if (!(o instanceof List) || ((List<Object>) o).size() < 4 || params.size() >= 6) continue;
                List<Object> p = (List<Object>) o;
                params.add(new Object[] {String.valueOf(p.get(0)), ((Number) p.get(1)).intValue(), ((Number) p.get(2)).intValue(),
                                         String.valueOf(p.get(3))});
                names.add(String.valueOf(p.get(0)));
            }
        String name = hd.get("name") == null ? "UNIT" : String.valueOf(hd.get("name"));
        byte[] h = header(1, module, payload.length, 0, 0, crc32(payload), name, params);
        return new LogueUnit(concat(h, payload), name, module, 1, names);
    }

    /* ------------------------------------------------------------------------------------------ mkII --- */
    static LogueUnit mk2(byte[] elf) throws Fm1Exception {
        if (elf.length < 52 || elf[0] != 0x7F || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 1 || u16(elf, 18) != 40)
            throw new Fm1Exception("unit", "不是 ARM 的 ELF32 單元檔");
        int phoff = u32(elf, 28), phent = u16(elf, 42), phnum = u16(elf, 44), dyn = -1, size = 0, memsz = 0;
        List<int[]> loads = new ArrayList<>();
        for (int i = 0; i < phnum; i++) {
            int o = phoff + i * phent;
            if (o + 32 > elf.length) throw new Fm1Exception("unit", "單元檔不完整");
            int type = u32(elf, o), off = u32(elf, o + 4), va = u32(elf, o + 8), fsz = u32(elf, o + 16), msz = u32(elf, o + 20);
            if (type == 1) {
                if (va < 0 || fsz < 0 || off < 0 || off + fsz > elf.length || va + fsz > 1 << 20)
                    throw new Fm1Exception("unit", "單元檔的區段不對");
                loads.add(new int[] {off, va, fsz, msz});
                size = Math.max(size, va + fsz);
                memsz = Math.max(memsz, va + msz);
            } else if (type == 2) {
                dyn = va;
            }
        }
        if (loads.isEmpty() || dyn < 0) throw new Fm1Exception("unit", "單元檔沒有可載入的區段");
        byte[] img = new byte[size];
        for (int[] l : loads) System.arraycopy(elf, l[0], img, l[1], l[2]);
        // the unit_header through the dynamic symbols (as CHAIN-1's lg_peek does)
        int uh = header2(img, dyn);
        if (uh < 0) throw new Fm1Exception("unit", "找不到 unit_header：不是 logue SDK 2 的單元");
        int target = u32(img, uh + 4);
        if ((target >>> 8) != 5) throw new Fm1Exception("unit", "這個單元不是給 NTS-1 mkII 的");
        int module = target & 0xFF;
        if (module < 1 || module > 4) throw new Fm1Exception("unit", "不支援的單元類型");
        String name = cstr(img, uh + 24, 20);
        int np = Math.min(11, u32(img, uh + 52));
        List<String> names = new ArrayList<>();
        for (int i = 0; i < np; i++) names.add(cstr(img, uh + 56 + 32 * i + 10, 22));
        byte[] h = header(2, 0, img.length, memsz, dyn, crc32(img), "", new ArrayList<>());
        return new LogueUnit(concat(h, img), name, module, 2, names);
    }

    static String cstr(byte[] b, int o, int max) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < max && o + i < b.length && b[o + i] != 0; i++) s.append((char) (b[o + i] & 0xFF));
        return s.toString();
    }

    static int header2(byte[] img, int dyn) {
        int hash = 0, symtab = 0, strtab = 0;
        for (int d = dyn; d + 8 <= img.length && u32(img, d) != 0; d += 8) {
            int tag = u32(img, d), val = u32(img, d + 4);
            if (tag == 4) hash = val;
            if (tag == 5) strtab = val;
            if (tag == 6) symtab = val;
        }
        if (hash <= 0 || hash + 8 > img.length || symtab <= 0 || strtab <= 0) return -1;
        int nsym = u32(img, hash + 4);
        for (int i = 1; i < nsym && symtab + 16 * i + 16 <= img.length; i++) {
            int s = symtab + 16 * i, nm = strtab + u32(img, s), v = u32(img, s + 4);
            if (u16(img, s + 14) != 0 && "unit_header".equals(cstr(img, nm, 12)) && v + 56 + 32 * 11 <= img.length) return v;
        }
        return -1;
    }

    /* ------------------------------------------------------------------------------------------ files --- */
    /** a .ntkdigunit (zip), a .nts1mkiiunit (ELF), or an .lgu as it is */
    public static LogueUnit pack(byte[] file) throws Fm1Exception {
        if (file.length >= 4 && file[0] == 'L' && file[1] == 'G' && file[2] == 'U' && file[3] == '1') {
            int fmt = file[4], module = file[5];
            return new LogueUnit(file, fmt == 1 ? cstr(file, 24, 20) : "UNIT", module, fmt, new ArrayList<>());
        }
        if (file.length >= 4 && file[0] == 0x7F && file[1] == 'E') return mk2(file);
        if (file.length >= 2 && file[0] == 'P' && file[1] == 'K') {
            byte[] payload = null;
            String manifest = null;
            try (ZipInputStream z = new ZipInputStream(new ByteArrayInputStream(file))) {
                for (ZipEntry e; (e = z.getNextEntry()) != null; ) {
                    if (e.isDirectory()) continue;
                    ByteArrayOutputStream o = new ByteArrayOutputStream();
                    byte[] buf = new byte[8192];
                    for (int n; (n = z.read(buf)) > 0; ) {
                        o.write(buf, 0, n);
                        if (o.size() > 1 << 20) throw new Fm1Exception("unit", "單元檔太大");
                    }
                    if (e.getName().endsWith("payload.bin")) payload = o.toByteArray();
                    if (e.getName().endsWith("manifest.json")) manifest = new String(o.toByteArray(), StandardCharsets.UTF_8);
                }
            } catch (Fm1Exception x) {
                throw x;
            } catch (Exception x) {
                throw new Fm1Exception("unit", "單元檔（zip）讀不開");
            }
            if (payload == null || manifest == null) throw new Fm1Exception("unit", "單元檔裡沒有 payload.bin / manifest.json");
            return mk1(payload, manifest);
        }
        throw new Fm1Exception("unit", "不是 logue 單元檔（.ntkdigunit / .nts1mkiiunit）");
    }
}
