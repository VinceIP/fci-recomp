# Button prompts

Images: default.xbe and the font files of each title. Found 2026-11-12.

Menus and hints write button tokens (`{A}`, `{BL}`, `{TRIGR}` ...) that a replacement table turns into
private-use characters of the font "Geometr706 BdCn BT Bold 20" (Instincts `sub_001A7010`, token table
`0x636AA0`, character table `0x5F3518`; Evolution's tables at `0x4EE570` and `0x4A7258`). The first 31
tokens are the buttons and map to the same characters in both titles.

The font is a Magma `.mft` (glyph records from 0x8F, 23 bytes each: u16 character; signed bytes width,
height, x and y offsets, advance; four f32 whose integer part is the page and fraction the texture
coordinate) with `.xbt` pages: DXT3, a 32-byte header (magic 0x03040100, width, height, size). All button
glyphs are on page 1 (256 x 128) except Evolution's `{TRIGG}` on page 2.

`src/xbr/fci_glyphs.cpp` recognises a page as it is uploaded by the FNV-1a hash of its data as the file
holds it (the game copies it unchanged), decodes the DXT3 blocks under each button glyph, draws over them
and encodes them again. Guest memory keeps the original. What it draws:

- pad: modern face buttons (A green, B red, X blue, Y yellow), LB and RB for the black and white buttons,
  LT and RT, the sticks, Menu and View for Start and Back, a d-pad with the named directions lit;
- mouse and keyboard: the first input bound to the control, as a keycap or a mouse with the button lit;
  the left stick shows the four movement keys, the right stick the mouse;
- typing into the on-screen keyboard: Enter, Esc, Space and Backspace for A, B, X and Y.

The device is the one `fci_input_on_kbm` reports (the same switch as multiplayer aim assist). When it or
the typing state changes, the page is marked dirty by its address and uploaded, so drawn, again.

Checked with scripted runs: both titles' pages recognised; the virtual keyboard's prompts drawn for the
pad, then as keycaps after a key press, then as the typing keys while typing; the popup over it shows the
plain bindings. Switching back to the pad needs a real pad (the scripted pad bypasses the activity hook).
