// SPDX-License-Identifier: MPL-2.0
// The installer's view of MIDI: the ports there are, and a link to one (SysEx out, SysEx in as whole messages).
package fm1.core;

import java.util.List;

public interface Midi {
    /** the MIDI ports there are now (USB devices with a MIDI interface) */
    List<String> ports();

    /** a link to one; null when it cannot be opened (no permission, gone) */
    Link open(String port);

    interface Link {
        /** one SysEx message (F0 .. F7); false when the port is gone */
        boolean send(byte[] sysex);

        /** the next SysEx message received, or null after ms */
        byte[] read(long ms);

        void drain();

        /** the device is gone */
        boolean lost();

        void close();
    }
}
