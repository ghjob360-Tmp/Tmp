// SPDX-License-Identifier: MPL-2.0
package fm1.core;

/** a stop with a code (fm1_install.py's InstallError codes) and a message for the person */
public class Fm1Exception extends Exception {
    public final String code;

    public Fm1Exception(String code, String msg) {
        super(msg);
        this.code = code;
    }
}
