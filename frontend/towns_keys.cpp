/* Retro-Towns - the machine's keyboard: the host table and the drawn layout.
 *
 * See towns_keys.h for why the TOWNS key numbers are copied rather than
 * included.  Nothing in this file knows about SDL windows or ImGui - it is two
 * tables and a lookup, which is what lets the virtual keyboard, the physical
 * keyboard and (later) the settings page all agree on what a key is.
 */
#include "towns_keys.h"

#include <SDL3/SDL_stdinc.h>

namespace towns
{

int jis_from_scancode(SDL_Scancode sc)
{
	switch(sc)
	{
	case SDL_SCANCODE_ESCAPE:     return JIS_ESC;
	case SDL_SCANCODE_1:          return JIS_1;
	case SDL_SCANCODE_2:          return JIS_2;
	case SDL_SCANCODE_3:          return JIS_3;
	case SDL_SCANCODE_4:          return JIS_4;
	case SDL_SCANCODE_5:          return JIS_5;
	case SDL_SCANCODE_6:          return JIS_6;
	case SDL_SCANCODE_7:          return JIS_7;
	case SDL_SCANCODE_8:          return JIS_8;
	case SDL_SCANCODE_9:          return JIS_9;
	case SDL_SCANCODE_0:          return JIS_0;
	case SDL_SCANCODE_MINUS:      return JIS_MINUS;
	case SDL_SCANCODE_EQUALS:     return JIS_MINUS;   /* - and = share a key   */
	case SDL_SCANCODE_GRAVE:      return JIS_HAT;     /* ^ and ~               */
	case SDL_SCANCODE_BACKSPACE:  return JIS_BACKSPACE;

	case SDL_SCANCODE_TAB:        return JIS_TAB;
	case SDL_SCANCODE_Q:          return JIS_Q;
	case SDL_SCANCODE_W:          return JIS_W;
	case SDL_SCANCODE_E:          return JIS_E;
	case SDL_SCANCODE_R:          return JIS_R;
	case SDL_SCANCODE_T:          return JIS_T;
	case SDL_SCANCODE_Y:          return JIS_Y;
	case SDL_SCANCODE_U:          return JIS_U;
	case SDL_SCANCODE_I:          return JIS_I;
	case SDL_SCANCODE_O:          return JIS_O;
	case SDL_SCANCODE_P:          return JIS_P;
	case SDL_SCANCODE_LEFTBRACKET:return JIS_LBRACKET; /* [ and {              */
	case SDL_SCANCODE_RIGHTBRACKET:return JIS_RBRACKET;
	case SDL_SCANCODE_RETURN:     return JIS_RETURN;

	case SDL_SCANCODE_CAPSLOCK:   return JIS_CAPS;
	case SDL_SCANCODE_A:          return JIS_A;
	case SDL_SCANCODE_S:          return JIS_S;
	case SDL_SCANCODE_D:          return JIS_D;
	case SDL_SCANCODE_F:          return JIS_F;
	case SDL_SCANCODE_G:          return JIS_G;
	case SDL_SCANCODE_H:          return JIS_H;
	case SDL_SCANCODE_J:          return JIS_J;
	case SDL_SCANCODE_K:          return JIS_K;
	case SDL_SCANCODE_L:          return JIS_L;
	case SDL_SCANCODE_SEMICOLON:  return JIS_SEMICOLON;
	case SDL_SCANCODE_APOSTROPHE: return JIS_QUOTE;
	case SDL_SCANCODE_BACKSLASH:  return JIS_BACKSLASH; /* ¥ and |             */
	case SDL_SCANCODE_NONUSBACKSLASH: return JIS_BACKSLASH;

	case SDL_SCANCODE_Z:          return JIS_Z;
	case SDL_SCANCODE_X:          return JIS_X;
	case SDL_SCANCODE_C:          return JIS_C;
	case SDL_SCANCODE_V:          return JIS_V;
	case SDL_SCANCODE_B:          return JIS_B;
	case SDL_SCANCODE_N:          return JIS_N;
	case SDL_SCANCODE_M:          return JIS_M;
	case SDL_SCANCODE_COMMA:      return JIS_COMMA;
	case SDL_SCANCODE_PERIOD:     return JIS_DOT;
	case SDL_SCANCODE_SLASH:      return JIS_SLASH;
	case SDL_SCANCODE_SPACE:      return JIS_SPACE;

	case SDL_SCANCODE_LSHIFT:     return JIS_SHIFT;
	case SDL_SCANCODE_RSHIFT:     return JIS_SHIFT;
	case SDL_SCANCODE_LCTRL:      return JIS_CTRL;
	case SDL_SCANCODE_RCTRL:      return JIS_CTRL;
	/* The TOWNS had no Windows key until someone wired one to the ALT code, so
	 * the host's meta keys are that key rather than nothing. */
	case SDL_SCANCODE_LALT:       return JIS_ALT;
	case SDL_SCANCODE_RALT:       return JIS_ALT;
	case SDL_SCANCODE_LGUI:       return JIS_ALT;
	case SDL_SCANCODE_RGUI:       return JIS_ALT;

	case SDL_SCANCODE_F1:         return JIS_PF01;
	case SDL_SCANCODE_F2:         return JIS_PF02;
	case SDL_SCANCODE_F3:         return JIS_PF03;
	case SDL_SCANCODE_F4:         return JIS_PF04;
	case SDL_SCANCODE_F5:         return JIS_PF05;
	case SDL_SCANCODE_F6:         return JIS_PF06;
	case SDL_SCANCODE_F7:         return JIS_PF07;
	case SDL_SCANCODE_F8:         return JIS_PF08;
	case SDL_SCANCODE_F9:         return JIS_PF09;
	case SDL_SCANCODE_F10:        return JIS_PF10;
	case SDL_SCANCODE_F11:        return JIS_PF11;
	case SDL_SCANCODE_F12:        return JIS_PF12;

	case SDL_SCANCODE_INSERT:     return JIS_INSERT;
	case SDL_SCANCODE_DELETE:     return JIS_DELETE;
	case SDL_SCANCODE_HOME:       return JIS_HOME;
	case SDL_SCANCODE_END:        return JIS_NEXT;     /* the TOWNS NEXT key   */
	case SDL_SCANCODE_PAGEUP:     return JIS_PREV;
	case SDL_SCANCODE_PAGEDOWN:   return JIS_NEXT;
	case SDL_SCANCODE_UP:         return JIS_UP;
	case SDL_SCANCODE_DOWN:       return JIS_DOWN;
	case SDL_SCANCODE_LEFT:       return JIS_LEFT;
	case SDL_SCANCODE_RIGHT:      return JIS_RIGHT;

	case SDL_SCANCODE_KP_0:       return JIS_NUM_0;
	case SDL_SCANCODE_KP_1:       return JIS_NUM_1;
	case SDL_SCANCODE_KP_2:       return JIS_NUM_2;
	case SDL_SCANCODE_KP_3:       return JIS_NUM_3;
	case SDL_SCANCODE_KP_4:       return JIS_NUM_4;
	case SDL_SCANCODE_KP_5:       return JIS_NUM_5;
	case SDL_SCANCODE_KP_6:       return JIS_NUM_6;
	case SDL_SCANCODE_KP_7:       return JIS_NUM_7;
	case SDL_SCANCODE_KP_8:       return JIS_NUM_8;
	case SDL_SCANCODE_KP_9:       return JIS_NUM_9;
	case SDL_SCANCODE_KP_PERIOD:  return JIS_NUM_DOT;
	case SDL_SCANCODE_KP_COMMA:   return JIS_NUM_000;
	case SDL_SCANCODE_KP_ENTER:   return JIS_NUM_RETURN;
	case SDL_SCANCODE_KP_PLUS:    return JIS_NUM_PLUS;
	case SDL_SCANCODE_KP_MINUS:   return JIS_NUM_MINUS;
	case SDL_SCANCODE_KP_MULTIPLY:return JIS_NUM_STAR;
	case SDL_SCANCODE_KP_DIVIDE:  return JIS_NUM_SLASH;
	case SDL_SCANCODE_KP_EQUALS:  return JIS_NUM_EQUAL;

	default:                      return JIS_NULL;
	}
}

namespace
{
	KbKey key(int jis,const char *label,float wide=1.0f,const char *alt="")
	{
		KbKey k;
		k.jis=jis;
		k.wide=wide;
		SDL_strlcpy(k.label,label,sizeof(k.label));
		SDL_strlcpy(k.alt,alt,sizeof(k.alt));
		return k;
	}
} /* namespace */

const std::vector <std::vector <KbKey>> &kb_layout()
{
	/* The FM TOWNS keyboard, in the order a row-based renderer wants it.  The
	 * main block is JIS - which is why the key after P is @ and not the bracket
	 * an ANSI keyboard puts there - and the machine's own extras are all here,
	 * because reaching them is the only reason to draw a keyboard at all:
	 * COPY and BREAK for the TBIOS, the kana keys, and the ten function keys the
	 * keyboard has (PF11 and PF12 are the two the Marty's layout puts to the
	 * right of the block).
	 *
	 * The cursor cluster and the numeric pad are rows underneath rather than a
	 * block to the right.  A real keyboard has them beside the letters; this has
	 * them below, and every code is still reachable. */
	static const std::vector <std::vector <KbKey>> rows=
	{
		{
			key(JIS_ESC,"ESC"),
			key(JIS_PF01,"F1"),key(JIS_PF02,"F2"),key(JIS_PF03,"F3"),
			key(JIS_PF04,"F4"),key(JIS_PF05,"F5"),
			key(JIS_PF06,"F6"),key(JIS_PF07,"F7"),key(JIS_PF08,"F8"),
			key(JIS_PF09,"F9"),key(JIS_PF10,"F10"),
			key(JIS_COPY,"COPY"),key(JIS_BREAK,"BREAK"),
			key(JIS_PF11,"F11"),key(JIS_PF12,"F12")
		},
		{
			key(JIS_1,"1"),key(JIS_2,"2"),key(JIS_3,"3"),key(JIS_4,"4"),
			key(JIS_5,"5"),key(JIS_6,"6"),key(JIS_7,"7"),key(JIS_8,"8"),
			key(JIS_9,"9"),key(JIS_0,"0"),
			key(JIS_MINUS,"-",1.0f,"="),
			key(JIS_HAT,"^",1.0f,"~"),
			key(JIS_BACKSLASH,"\\",1.0f,"|"),
			key(JIS_BACKSPACE,"BS",2.0f)
		},
		{
			key(JIS_TAB,"TAB",1.5f),
			key(JIS_Q,"Q"),key(JIS_W,"W"),key(JIS_E,"E"),key(JIS_R,"R"),
			key(JIS_T,"T"),key(JIS_Y,"Y"),key(JIS_U,"U"),key(JIS_I,"I"),
			key(JIS_O,"O"),key(JIS_P,"P"),
			key(JIS_AT,"@"),key(JIS_LBRACKET,"["),
			key(JIS_RETURN,"RETURN",2.0f)
		},
		{
			key(JIS_CAPS,"CAPS",2.0f),
			key(JIS_A,"A"),key(JIS_S,"S"),key(JIS_D,"D"),key(JIS_F,"F"),
			key(JIS_G,"G"),key(JIS_H,"H"),key(JIS_J,"J"),key(JIS_K,"K"),
			key(JIS_L,"L"),
			key(JIS_SEMICOLON,";"),key(JIS_COLON,":"),key(JIS_RBRACKET,"]"),
		},
		{
			key(JIS_SHIFT,"SHIFT",2.5f),
			key(JIS_Z,"Z"),key(JIS_X,"X"),key(JIS_C,"C"),key(JIS_V,"V"),
			key(JIS_B,"B"),key(JIS_N,"N"),key(JIS_M,"M"),
			key(JIS_COMMA,","),key(JIS_DOT,"."),key(JIS_SLASH,"/"),
			key(JIS_QUOTE,"'"),
			key(JIS_SHIFT,"SHIFT",1.5f)
		},
		{
			key(JIS_CTRL,"CTRL",1.25f),
			key(JIS_KANA_KANJI,"KANA\nKANJI",1.25f),
			key(JIS_HIRAGANA,"HIRA",1.25f),
			key(JIS_SPACE,"SPACE",6.0f),
			key(JIS_KATAKANA,"KATA",1.25f),
			key(JIS_CONVERT,"HENKEN",1.25f),
			key(JIS_NO_CONVERT,"MUHEN",1.25f),
			key(JIS_ALT,"ALT",1.25f)
		},
		{
			key(JIS_CANCEL,"CANCEL"),key(JIS_EXECUTE,"EXEC"),
			key(JIS_INSERT,"INS"),key(JIS_HOME,"HOME"),key(JIS_PREV,"PREV"),
			key(JIS_DELETE,"DEL"),key(JIS_NEXT,"NEXT"),
			key(JIS_ERASE_WORD,"X-WRD"),key(JIS_ADD_WORD,"ADD-W"),
			key(JIS_KANJI_DIC,"DIC"),key(JIS_CHAR_PITCH,"PITCH")
		},
		{
			key(JIS_LEFT,"<"),key(JIS_DOWN,"v"),key(JIS_RIGHT,">"),key(JIS_UP,"^"),
			key(JIS_NUM_7,"7"),key(JIS_NUM_8,"8"),key(JIS_NUM_9,"9"),
			key(JIS_NUM_STAR,"*"),
			key(JIS_NUM_4,"4"),key(JIS_NUM_5,"5"),key(JIS_NUM_6,"6"),
			key(JIS_NUM_SLASH,"/"),
			key(JIS_NUM_1,"1"),key(JIS_NUM_2,"2"),key(JIS_NUM_3,"3"),
			key(JIS_NUM_MINUS,"-"),
			key(JIS_NUM_0,"0"),key(JIS_NUM_000,"00"),
			key(JIS_NUM_DOT,"."),key(JIS_NUM_PLUS,"+"),
			key(JIS_NUM_EQUAL,"="),key(JIS_NUM_RETURN,"ENT",2.0f)
		}
	};
	return rows;
}

} /* namespace towns */
