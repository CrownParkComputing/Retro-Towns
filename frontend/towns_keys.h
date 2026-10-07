/* Retro-Towns - the machine's keyboard.
 *
 * The FM TOWNS took a JIS keyboard with a block of Fujitsu-specific keys above
 * it, and the core wants those keys by their own numbers: `ftowns_key()` takes a
 * TOWNS_JISKEY_* code, not a host key code.  The values are copied from
 * src/towns/townsdef/townsdef.h rather than included, because the front end is
 * deliberately not allowed to pull a Tsugaru header into its own translation
 * units - it talks to the core through the C bridge and nothing else, and that
 * is what keeps the two halves compilable with different toolchains on Android.
 * If the core ever renumbers these, the bridge's own copy is the thing to
 * check against.
 *
 * Two tables live here:
 *
 *   towns::jis_from_scancode()  a host keyboard key to the TOWNS key it becomes
 *   towns::kb_layout()          the rows drawn on the virtual keyboard
 *
 * The second is the FM TOWNS layout rather than a generic QWERTY grid, because
 * the point of a virtual keyboard is to reach the keys a laptop does not have -
 * CALC/EISU, KANA, the ten function keys the machine actually has, and the
 * cursor block with PREV/NEXT on it.
 */
#ifndef TOWNS_KEYS_H
#define TOWNS_KEYS_H

#include <SDL3/SDL_scancode.h>

#include <vector>

namespace towns
{
	/* TOWNS_JISKEY_* as the core numbers them. */
	enum JisKey : int
	{
		JIS_NULL       =0x00,

		JIS_ESC        =0x01,
		JIS_1=0x02, JIS_2=0x03, JIS_3=0x04, JIS_4=0x05, JIS_5=0x06,
		JIS_6=0x07, JIS_7=0x08, JIS_8=0x09, JIS_9=0x0A, JIS_0=0x0B,
		JIS_MINUS      =0x0C,   /* - and =                              */
		JIS_HAT        =0x0D,   /* ^ and ~ (JIS)                        */
		JIS_BACKSLASH  =0x0E,   /* ¥ and |                              */
		JIS_BACKSPACE  =0x0F,

		JIS_TAB        =0x10,
		JIS_Q=0x11, JIS_W=0x12, JIS_E=0x13, JIS_R=0x14, JIS_T=0x15,
		JIS_Y=0x16, JIS_U=0x17, JIS_I=0x18, JIS_O=0x19, JIS_P=0x1A,
		JIS_AT         =0x1B,   /* @ and `                              */
		JIS_LBRACKET   =0x1C,
		JIS_RETURN     =0x1D,

		JIS_A=0x1E, JIS_S=0x1F, JIS_D=0x20, JIS_F=0x21, JIS_G=0x22,
		JIS_H=0x23, JIS_J=0x24, JIS_K=0x25, JIS_L=0x26,
		JIS_SEMICOLON  =0x27,
		JIS_COLON      =0x28,
		JIS_RBRACKET   =0x29,

		JIS_Z=0x2A, JIS_X=0x2B, JIS_C=0x2C, JIS_V=0x2D, JIS_B=0x2E,
		JIS_N=0x2F, JIS_M=0x30,
		JIS_COMMA      =0x31,
		JIS_DOT        =0x32,
		JIS_SLASH      =0x33,
		JIS_QUOTE      =0x34,   /* ' and "                              */
		JIS_SPACE      =0x35,

		JIS_NUM_STAR   =0x36,
		JIS_NUM_SLASH  =0x37,
		JIS_NUM_PLUS   =0x38,
		JIS_NUM_MINUS  =0x39,
		JIS_NUM_7=0x3A, JIS_NUM_8=0x3B, JIS_NUM_9=0x3C,
		JIS_NUM_EQUAL  =0x3D,
		JIS_NUM_4=0x3E, JIS_NUM_5=0x3F, JIS_NUM_6=0x40,
		JIS_NUM_1=0x42, JIS_NUM_2=0x43, JIS_NUM_3=0x44,
		JIS_NUM_RETURN =0x45,
		JIS_NUM_0      =0x46,
		JIS_NUM_DOT    =0x47,
		JIS_NUM_000    =0x4A,   /* the TOWNS pad's double zero */

		JIS_INSERT     =0x48,
		JIS_DELETE     =0x4B,
		JIS_HOME       =0x4E,
		JIS_UP         =0x4D,
		JIS_LEFT       =0x4F,
		JIS_DOWN       =0x50,
		JIS_RIGHT      =0x51,

		JIS_CTRL       =0x52,
		JIS_SHIFT      =0x53,
		JIS_CAPS       =0x55,
		JIS_HIRAGANA   =0x56,
		JIS_NO_CONVERT =0x57,
		JIS_CONVERT    =0x58,
		JIS_KANA_KANJI =0x59,
		JIS_KATAKANA   =0x5A,

		JIS_PF12       =0x5B,
		JIS_ALT        =0x5C,   /* the TOWNS "WINDOWS" key, per WINDY   */
		JIS_PF01=0x5D, JIS_PF02=0x5E, JIS_PF03=0x5F, JIS_PF04=0x60,
		JIS_PF05=0x61, JIS_PF06=0x62, JIS_PF07=0x63, JIS_PF08=0x64,
		JIS_PF09=0x65, JIS_PF10=0x66,
		JIS_PF11       =0x69,
		JIS_KANJI_DIC  =0x6B,
		JIS_ERASE_WORD =0x6C,
		JIS_ADD_WORD   =0x6D,
		JIS_PREV       =0x6E,
		JIS_NEXT       =0x70,
		JIS_CHAR_PITCH =0x71,
		JIS_CANCEL     =0x72,
		JIS_EXECUTE    =0x73,
		JIS_PF13=0x74, JIS_PF14=0x75, JIS_PF15=0x76, JIS_PF16=0x77,
		JIS_PF17=0x78, JIS_PF18=0x79, JIS_PF19=0x7A, JIS_PF20=0x7B,

		JIS_BREAK      =0x7C,
		JIS_COPY       =0x7D
	};

	/* One key on the virtual keyboard.  `jis` is what to press; `label` is what
	 * to draw; `wide` is in key widths, so a key can be 1.25 or 6.25 times a
	 * normal one the way a real keyboard is. */
	struct KbKey
	{
		int   jis;
		char  label[12];
		float wide=1.0f;
		/* The second legend a JIS key carries, drawn small in the corner: the
		 * shifted thing, or the kana. */
		char  alt[8]="";
	};

	/* The host's key, as SDL numbers it, to the TOWNS key it stands for.
	 * Returns JIS_NULL for a key with no TOWNS equivalent - which includes
	 * every key the app has already claimed for itself. */
	int jis_from_scancode(SDL_Scancode sc);

	/* The rows of the FM TOWNS keyboard, left to right, top to bottom.  Drawn
	 * from this so the layout and the codes cannot drift apart. */
	const std::vector <std::vector <KbKey>> &kb_layout();
}

#endif /* TOWNS_KEYS_H */
