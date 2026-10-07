/* towns_pad_map.cpp -- see towns_pad_map.h. */

#include "towns_pad_map.h"

#include <cstring>

namespace padmap {

const Entry kEntries[SIG_COUNT]={
    {"UP",    "pad_up",   "Jump, look up, steer up",       FTOWNS_PAD_UP   },
    {"DOWN",  "pad_down", "Crouch, steer down",            FTOWNS_PAD_DOWN },
    {"LEFT",  "pad_left", "Steer left",                    FTOWNS_PAD_LEFT },
    {"RIGHT", "pad_right","Steer right",                   FTOWNS_PAD_RIGHT},
    {"A",     "pad_a",    "First action button",           FTOWNS_PAD_A    },
    {"B",     "pad_b",    "Second action button",          FTOWNS_PAD_B    },
    {"RUN",   "pad_run",  "Start, confirm, run",           FTOWNS_PAD_RUN  },
    {"PAUSE", "pad_pause","Pause the machine",             FTOWNS_PAD_PAUSE},
    {"ZOOM",  "pad_zoom", "Screen magnify; some games use it as a third button",
                                                            FTOWNS_PAD_ZOOM },
};

namespace {

/* Authored pixels in units of ImGui's 13-pixel default font, scaled to whatever
 * the interface is actually drawn at - the same convention the host uses. */
static float px(float authored)
{
    return authored*(ImGui::GetFontSize()/13.0f);
}

/* A binding is stored as a bare SDL control name, so the file reads as
 * `pad_a=south` rather than as a tagged union.  The two namespaces do not
 * overlap - gamepad buttons are "south", "dpadup", scancodes are "A",
 * "Return" - so a name is resolved by asking SDL which list it is in.
 *
 * Both lookups are table scans over a few dozen entries, and they run once per
 * signal per frame, which is why there is no cache here. */
int button_of(const std::string &name)
{
    if(name.empty())
    {
        return -1;
    }
    /* Keyboard bindings are prefixed so a host key like "A" cannot be read as
     * the gamepad's "a" (south) button - the two namespaces do overlap on the
     * face-button letters. */
    if(0==name.compare(0,2,"k:"))
    {
        return -1;
    }
    return SDL_GetGamepadButtonFromString(name.c_str());
}

SDL_Scancode scancode_of(const std::string &name)
{
    if(name.empty())
    {
        return SDL_SCANCODE_UNKNOWN;
    }
    const char *n=name.c_str();
    if(0==name.compare(0,2,"k:"))
    {
        n+=2;
    }
    const SDL_Scancode sc=SDL_GetScancodeFromName(n);
    /* SDL_GetScancodeFromName sets an error for a name it does not know, and
     * the next SDL call would report that error as its own. */
    if(SDL_SCANCODE_UNKNOWN==sc)
    {
        SDL_ClearError();
    }
    return sc;
}

} /* namespace */

Map Map::defaults(void)
{
    Map m;
    /* SDL's own names, from SDL_GetGamepadStringForButton - "dpup" rather than
     * "dpadup", "a" rather than "south".  A name the library does not know
     * binds to nothing and says so by doing nothing at all, which is the worst
     * possible failure for a control scheme, so these are worth checking
     * against the library rather than against memory. */
    m.bind[SIG_UP]   ="dpup";
    m.bind[SIG_DOWN] ="dpdown";
    m.bind[SIG_LEFT] ="dpleft";
    m.bind[SIG_RIGHT]="dpright";
    m.bind[SIG_A]    ="a";
    m.bind[SIG_B]    ="b";
    /* START is RUN, not PAUSE.  Every arcade-style title asks for "push
     * start" and means the line that resumes the machine, and a player who
     * reaches for START to pause can read the on-screen label. */
    m.bind[SIG_RUN]  ="start";
    m.bind[SIG_PAUSE]="back";
    m.bind[SIG_ZOOM] ="rightshoulder";
    return m;
}

unsigned int Map::mask(SDL_Gamepad *pad) const
{
    unsigned int bits=0;
    if(nullptr==pad)
    {
        return 0;
    }
    for(int i=0; i<SIG_COUNT; ++i)
    {
        const int btn=button_of(bind[i]);
        if(0<=btn && SDL_GetGamepadButton(pad,(SDL_GamepadButton)btn))
        {
            bits|=kEntries[i].bit;
        }
    }
    return bits;
}

int Map::signal_for_key(SDL_Scancode sc) const
{
    if(SDL_SCANCODE_UNKNOWN==sc)
    {
        return -1;
    }
    for(int i=0; i<SIG_COUNT; ++i)
    {
        /* A binding that names a pad button is a pad button, even though SDL
         * also knows "a" and "b" as keys on the home row - which is exactly
         * why the defaults read as pad names.  Claiming them here would type
         * "a" into the machine and press pad A at the same time. */
        if(0<=button_of(bind[i]))
        {
            continue;
        }
        if(sc==scancode_of(bind[i]))
        {
            return i;
        }
    }
    return -1;
}

bool Map::valid(const std::string &text)
{
    return 0<=button_of(text) || SDL_SCANCODE_UNKNOWN!=scancode_of(text);
}

std::string Map::describe(int sig) const
{
    if(sig<0 || SIG_COUNT<=sig || bind[sig].empty())
    {
        return "Unbound";
    }
    if(0==bind[sig].compare(0,2,"k:"))
    {
        return "Key "+bind[sig].substr(2);
    }
    if(0<=button_of(bind[sig]))
    {
        return "Pad "+bind[sig];
    }
    return "Key "+bind[sig];
}

std::string Map::name_of(const SDL_Event &ev)
{
    switch(ev.type)
    {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        {
            const char *n=SDL_GetGamepadStringForButton((SDL_GamepadButton)ev.gbutton.button);
            return nullptr!=n ? std::string(n) : std::string();
        }
    case SDL_EVENT_KEY_DOWN:
        return "k:"+std::string(SDL_GetScancodeName(ev.key.scancode));
    default:
        return std::string();
    }
}

/* ---------------------------------------------------------------- */
/* The settings page                                                 */
/* ---------------------------------------------------------------- */

bool capture(Map &map,const SDL_Event &ev,int &armed)
{
    if(0>armed)
    {
        return false;
    }
    if(SDL_EVENT_KEY_DOWN==ev.type && SDL_SCANCODE_ESCAPE==ev.key.scancode)
    {
        armed=-1;
        return false;
    }
    if(SDL_EVENT_GAMEPAD_BUTTON_DOWN!=ev.type && SDL_EVENT_KEY_DOWN!=ev.type)
    {
        return false;
    }
    const std::string name=Map::name_of(ev);
    if(name.empty() || !Map::valid(name))
    {
        return false;
    }
    map.bind[armed]=name;
    armed=-1;
    return true;
}

bool editor(Map &map,int &armed)
{
    bool changed=false;

    /* Binding is picked from a dropdown now, so nothing is ever armed. */
    armed=-1;

    static const SDL_Scancode kKeyChoices[]={
        SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_RETURN,SDL_SCANCODE_SPACE,SDL_SCANCODE_LSHIFT,
        SDL_SCANCODE_LCTRL,SDL_SCANCODE_LALT,SDL_SCANCODE_ESCAPE,SDL_SCANCODE_TAB,
        SDL_SCANCODE_BACKSPACE,
        SDL_SCANCODE_A,SDL_SCANCODE_B,SDL_SCANCODE_C,SDL_SCANCODE_D,SDL_SCANCODE_E,
        SDL_SCANCODE_F,SDL_SCANCODE_G,SDL_SCANCODE_H,SDL_SCANCODE_I,SDL_SCANCODE_J,
        SDL_SCANCODE_K,SDL_SCANCODE_L,SDL_SCANCODE_M,SDL_SCANCODE_N,SDL_SCANCODE_O,
        SDL_SCANCODE_P,SDL_SCANCODE_Q,SDL_SCANCODE_R,SDL_SCANCODE_S,SDL_SCANCODE_T,
        SDL_SCANCODE_U,SDL_SCANCODE_V,SDL_SCANCODE_W,SDL_SCANCODE_X,SDL_SCANCODE_Y,
        SDL_SCANCODE_Z,
        SDL_SCANCODE_1,SDL_SCANCODE_2,SDL_SCANCODE_3,SDL_SCANCODE_4,SDL_SCANCODE_5,
        SDL_SCANCODE_6,SDL_SCANCODE_7,SDL_SCANCODE_8,SDL_SCANCODE_9,SDL_SCANCODE_0,
    };

    for(int i=0; i<SIG_COUNT; ++i)
    {
        ImGui::PushID(i);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%-6s",kEntries[i].label);
        ImGui::SameLine(px(70.0f));
        ImGui::TextDisabled("%s",kEntries[i].meaning);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x*0.52f+px(40.0f));

        const std::string preview=map.describe(i);
        if(ImGui::BeginCombo("##bind",preview.c_str()))
        {
            ImGui::TextDisabled("Pad");
            for(int b=SDL_GAMEPAD_BUTTON_SOUTH; b<=SDL_GAMEPAD_BUTTON_DPAD_RIGHT; ++b)
            {
                const char *n=SDL_GetGamepadStringForButton((SDL_GamepadButton)b);
                if(nullptr==n)
                {
                    continue;
                }
                if(ImGui::Selectable(n,map.bind[i]==n))
                {
                    map.bind[i]=n;
                    changed=true;
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Keyboard");
            for(const SDL_Scancode sc : kKeyChoices)
            {
                const char *n=SDL_GetScancodeName(sc);
                if(nullptr==n || '\0'==*n)
                {
                    continue;
                }
                const std::string kn="k:"+std::string(n);
                if(ImGui::Selectable(n,map.bind[i]==kn))
                {
                    map.bind[i]=kn;
                    changed=true;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(map.bind[i].empty());
        if(ImGui::Button("Clear"))
        {
            map.bind[i].clear();
            changed=true;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    return changed;
}

/* ---------------------------------------------------------------- */
/* The picture                                                       */
/* ---------------------------------------------------------------- */

namespace {

const ImU32 kBody   =IM_COL32(0x2b,0x2b,0x31,255);
const ImU32 kEdge   =IM_COL32(0x8a,0x8f,0x98,255);
const ImU32 kKey    =IM_COL32(0x49,0x4e,0x58,255);
const ImU32 kText   =IM_COL32(0xe8,0xe8,0xe8,255);
const ImU32 kDim    =IM_COL32(0x9a,0x9a,0x9a,255);
const ImU32 kArm    =IM_COL32(0xff,0xd4,0x5a,255);

/* One control, its label, and the binding underneath it. */
void key(ImDrawList *dl,const ImVec2 &centre,const ImVec2 &half,bool round,
         int sig,const Map &map,int armed)
{
    const bool lit=(armed==sig);
    const ImU32 edge=lit?kArm:kEdge;
    if(round)
    {
        dl->AddCircleFilled(centre,half.x,kKey,0);
        dl->AddCircle(centre,half.x,edge,lit?0:24,lit?3.0f:1.5f);
    }
    else
    {
        dl->AddRectFilled(ImVec2(centre.x-half.x,centre.y-half.y),
                          ImVec2(centre.x+half.x,centre.y+half.y),kKey,4.0f);
        dl->AddRect(ImVec2(centre.x-half.x,centre.y-half.y),
                    ImVec2(centre.x+half.x,centre.y+half.y),edge,4.0f,0,
                    lit?3.0f:1.5f);
    }

    const ImVec2 name=ImGui::CalcTextSize(kEntries[sig].label);
    /* The signal is what the machine sees, so it goes on the button; the
     * binding is what the player pressed, and it goes underneath where there
     * is room for a whole word. */
    dl->AddText(ImVec2(centre.x-name.x*0.5f,centre.y-name.y*0.5f),kText,
                kEntries[sig].label);
    const std::string bound=map.describe(sig);
    const ImVec2 bs=ImGui::CalcTextSize(bound.c_str());
    dl->AddText(ImVec2(centre.x-bs.x*0.5f,centre.y+half.y+3.0f),
                lit?kArm:kDim,bound.c_str());
}

} /* namespace */

void draw_controller(ImDrawList *dl,const ImVec2 &pos,const ImVec2 &size,
                     const Map &map,int armed)
{
    const float s=(size.x<120.0f || size.y<80.0f) ? 0.0f : 1.0f;
    if(0.0f==s)
    {
        return;
    }
    const ImVec2 a=pos;
    const ImVec2 b(pos.x+size.x,pos.y+size.y);

    /* The body: a wide slab with the lever on the left and the buttons on the
     * right, which is how the controller was laid out. */
    dl->AddRectFilled(a,b,kBody,10.0f);
    dl->AddRect(a,b,kEdge,10.0f,0,1.5f);

    const float w=size.x,h=size.y;

    /* The lever, drawn as a cross so the four directions read as one control
     * the way they do on the hardware. */
    const ImVec2 dp(a.x+w*0.22f,a.y+h*0.44f);
    const float arm_w=w*0.055f,arm_l=h*0.17f;
    dl->AddRectFilled(ImVec2(dp.x-arm_w,dp.y-arm_l),ImVec2(dp.x+arm_w,dp.y+arm_l),
                      kKey,4.0f);
    dl->AddRectFilled(ImVec2(dp.x-arm_l,dp.y-arm_w),ImVec2(dp.x+arm_l,dp.y+arm_w),
                      kKey,4.0f);
    key(dl,ImVec2(dp.x,dp.y-arm_l-arm_w*0.6f),ImVec2(arm_w*0.9f,arm_w*0.9f),true,
        SIG_UP,map,armed);
    key(dl,ImVec2(dp.x,dp.y+arm_l+arm_w*0.6f),ImVec2(arm_w*0.9f,arm_w*0.9f),true,
        SIG_DOWN,map,armed);
    key(dl,ImVec2(dp.x-arm_l-arm_w*0.6f,dp.y),ImVec2(arm_w*0.9f,arm_w*0.9f),true,
        SIG_LEFT,map,armed);
    key(dl,ImVec2(dp.x+arm_l+arm_w*0.6f,dp.y),ImVec2(arm_w*0.9f,arm_w*0.9f),true,
        SIG_RIGHT,map,armed);

    /* A and B, side by side, A nearest the lever. */
    const float r=h*0.085f;
    key(dl,ImVec2(a.x+w*0.68f,a.y+h*0.42f),ImVec2(r,r),true,SIG_A,map,armed);
    key(dl,ImVec2(a.x+w*0.84f,a.y+h*0.42f),ImVec2(r,r),true,SIG_B,map,armed);

    /* The three signals that came off the controller alongside the buttons. */
    const float pw=w*0.055f,ph=h*0.05f;
    key(dl,ImVec2(a.x+w*0.36f,a.y+h*0.80f),ImVec2(pw,ph),false,SIG_RUN,map,armed);
    key(dl,ImVec2(a.x+w*0.50f,a.y+h*0.80f),ImVec2(pw,ph),false,SIG_PAUSE,map,armed);
    key(dl,ImVec2(a.x+w*0.64f,a.y+h*0.80f),ImVec2(pw,ph),false,SIG_ZOOM,map,armed);
}

} /* namespace padmap */
