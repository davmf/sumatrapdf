/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"

#include "SvgIcons.h"
#include "Translations.h"

#include "CommentCards.h"

/*
A list of comment cards in collapsible groups, drawn by us:

  [ Page 6                    2 v ]
  +-------------------------------+
  | [] dxf  Highlight           o |
  | Remove this sentence.         |
  |  | bob  2026-10-05            |
  |  | agreed                     |
  | v Completed (bob)  2026-10-05 |
  | [Add Reply                  ] |  <- the selected card, an Edit
  +-------------------------------+
*/

constexpr int kMargin = 6;
constexpr int kPad = 8;
constexpr int kGap = 6;
constexpr int kLineGap = 3;
constexpr int kRadius = 4;
constexpr int kHeaderPadY = 5;
constexpr int kReplyIndent = 10;
constexpr int kSwatchDx = 8;
constexpr int kChevronDx = 8;
// a long comment is cut, the card popup shows it all
constexpr int kMaxTextDy = 400;
constexpr uint kTextFormat = DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX;
constexpr uint kLineFormat = DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER;

static Kind kindCommentCards = "commentCards";

CommentCardReply::~CommentCardReply() {
    str::Free(author);
    str::Free(date);
    str::Free(text);
}

CommentCard::~CommentCard() {
    str::Free(author);
    str::Free(kind);
    str::Free(date);
    str::Free(text);
    str::Free(status);
    DeleteVecMembers(replies);
}

CommentCardGroup::~CommentCardGroup() {
    str::Free(title);
    DeleteVecMembers(cards);
}

CommentCards::CommentCards() {
    kind = kindCommentCards;
}

CommentCards::~CommentCards() {
    delete replyEdit;
    DeleteVecMembers(groups);
}

static int Scale(CommentCards* w, int v) {
    return DpiScaleByDpi(DpiGetForHwnd(w->hwnd), v);
}

static HFONT BoldHFont(CommentCards* w) {
    return GetBoldPlatformFont(w->font)->GetHFont();
}

static int TextDx(HDC hdc, Str s, HFONT font) {
    return HdcMeasureText(hdc, s, 10000, DT_SINGLELINE | DT_NOPREFIX, font).dx;
}

// height of a line of the bold font, which is the taller
static int LineDy(CommentCards* w) {
    if (w->lineDy > 0) {
        return w->lineDy;
    }
    HDC hdc = GetDC(w->hwnd);
    HGDIOBJ prev = SelectObject(hdc, BoldHFont(w));
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, prev);
    ReleaseDC(w->hwnd, hdc);
    w->lineDy = tm.tmHeight;
    return w->lineDy;
}

static Color MutedCol(CommentCards* w) {
    float units = IsLightColor(w->bgCol) ? 100.0f : -90.0f;
    return AdjustLightness2(w->textCol, units);
}

static Color StatusCol(CommentCards* w, CommentCardStatus st) {
    bool light = IsLightColor(w->bgCol);
    switch (st) {
        case CommentCardStatus::Done:
            return light ? MkRgb(0x1e, 0x7e, 0x34) : MkRgb(0x6c, 0xcb, 0x7f);
        case CommentCardStatus::Declined:
            return light ? MkRgb(0xb0, 0x2a, 0x2a) : MkRgb(0xf0, 0x80, 0x80);
        default:
            return MutedCol(w);
    }
}

// "v" for a done thread, "x" for a declined one
static Str StatusMark(CommentCardStatus st) {
    switch (st) {
        case CommentCardStatus::Done:
            return StrL("\xE2\x9C\x93 ");
        case CommentCardStatus::Declined:
            return StrL("\xE2\x9C\x97 ");
        default:
            return {};
    }
}

static int TextDy(HDC hdc, Str s, int dx, HFONT font) {
    if (len(s) == 0) {
        return 0;
    }
    int dy = HdcMeasureText(hdc, s, dx, kTextFormat, font).dy;
    return std::min(dy, kMaxTextDy);
}

static int ReplyBoxDy(CommentCards* w) {
    return LineDy(w) + Scale(w, 8);
}

// the card's height for an inner width
static int CardDy(CommentCards* w, HDC hdc, CommentCard* c, int innerDx) {
    HFONT font = w->font->GetHFont();
    int lineDy = LineDy(w);
    int gap = Scale(w, kLineGap);
    int dy = Scale(w, kPad) + lineDy;
    if (len(c->text) > 0) {
        dy += gap + TextDy(hdc, c->text, innerDx, font);
    }
    int replyDx = innerDx - Scale(w, kReplyIndent);
    for (CommentCardReply* r : c->replies) {
        dy += gap * 2 + lineDy;
        if (len(r->text) > 0) {
            dy += gap + TextDy(hdc, r->text, replyDx, font);
        }
    }
    dy += gap * 2 + lineDy;
    if (c == w->selected && c->canReply) {
        dy += Scale(w, kGap) + ReplyBoxDy(w);
    }
    return dy + Scale(w, kPad);
}

void CommentCards::Relayout() {
    if (!hwnd || !font) {
        return;
    }
    Rect rc = HwndClientRect(hwnd);
    int m = Scale(this, kMargin);
    int gap = Scale(this, kGap);
    int pad = Scale(this, kPad);
    int dx = std::max(rc.dx - 2 * m, 1);
    int headerDy = LineDy(this) + 2 * Scale(this, kHeaderPadY);
    HDC hdc = GetDC(hwnd);
    int y = m;
    for (CommentCardGroup* g : groups) {
        g->rect = {m, y, dx, headerDy};
        y += headerDy + gap;
        if (g->collapsed) {
            continue;
        }
        for (CommentCard* c : g->cards) {
            int dy = CardDy(this, hdc, c, dx - 2 * pad);
            c->rect = {m, y, dx, dy};
            c->replyBox = {};
            if (c == selected && c->canReply) {
                int boxDy = ReplyBoxDy(this);
                c->replyBox = {m + pad, y + dy - pad - boxDy, dx - 2 * pad, boxDy};
            }
            y += dy + gap;
        }
    }
    ReleaseDC(hwnd, hdc);
    contentDy = y;
    UpdateScrollbar();
    Scroll(scrollY);
    PlaceReplyEdit();
    HwndRepaintNow(hwnd);
}

void CommentCards::UpdateScrollbar() {
    Rect rc = HwndClientRect(hwnd);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(contentDy - 1, 0);
    si.nPage = (UINT)std::max(rc.dy, 0);
    si.nPos = scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void CommentCards::Scroll(int y) {
    Rect rc = HwndClientRect(hwnd);
    int maxY = std::max(contentDy - rc.dy, 0);
    y = std::clamp(y, 0, maxY);
    if (y == scrollY) {
        return;
    }
    scrollY = y;
    UpdateScrollbar();
    PlaceReplyEdit();
    HwndScheduleRepaint(hwnd);
}

// r in content coords
void CommentCards::EnsureVisible(Rect r) {
    Rect rc = HwndClientRect(hwnd);
    if (r.y < scrollY) {
        Scroll(r.y - Scale(this, kMargin));
    } else if (r.y + r.dy > scrollY + rc.dy) {
        // a card taller than the view shows its top
        int y = r.y + r.dy - rc.dy + Scale(this, kMargin);
        Scroll(std::min(y, r.y - Scale(this, kMargin)));
    }
}

// the Edit sits over the selected card's reply box, hidden when that's off view
void CommentCards::PlaceReplyEdit() {
    if (!replyEdit) {
        return;
    }
    CommentCard* c = selected;
    bool show = c && c->canReply && !c->replyBox.IsEmpty() && !GroupOf(c)->collapsed;
    if (!show) {
        if (IsWindowVisible(replyEdit->hwnd)) {
            bool hadFocus = HwndIsFocused(replyEdit->hwnd);
            ShowWindow(replyEdit->hwnd, SW_HIDE);
            replyEdit->SetText(StrL(""));
            if (hadFocus) {
                HwndSetFocus(hwnd);
            }
        }
        return;
    }
    Rect r = c->replyBox;
    r.y -= scrollY;
    // inside the frame drawn around it
    r.Inflate(-1, -1);
    MoveWindow(replyEdit->hwnd, r.x, r.y, r.dx, r.dy, TRUE);
    ShowWindow(replyEdit->hwnd, SW_SHOWNOACTIVATE);
}

void CommentCards::FocusReply() {
    PlaceReplyEdit();
    if (replyEdit && IsWindowVisible(replyEdit->hwnd)) {
        EnsureVisible(selected->rect);
        HwndSetFocus(replyEdit->hwnd);
    }
}

CommentCardGroup* CommentCards::GroupOf(CommentCard* c) {
    for (CommentCardGroup* g : groups) {
        if (VecContains(g->cards, c)) {
            return g;
        }
    }
    return nullptr;
}

void CommentCards::SetGroups(Vec<CommentCardGroup*>& newGroups) {
    // the selection goes with the cards; the owner selects again
    selected = nullptr;
    DeleteVecMembers(groups);
    for (CommentCardGroup* g : newGroups) {
        VecAppend(groups, g);
    }
    VecReset(newGroups);
    if (replyEdit) {
        replyEdit->SetText(StrL(""));
    }
    Relayout();
}

void CommentCards::Select(CommentCard* c, bool scrollIntoView) {
    if (c != selected) {
        if (replyEdit) {
            replyEdit->SetText(StrL(""));
        }
        selected = c;
        Relayout();
    }
    if (c && scrollIntoView && !GroupOf(c)->collapsed) {
        EnsureVisible(c->rect);
    }
}

void CommentCards::SetCardColors(Color text, Color bg) {
    textCol = text;
    bgCol = bg;
    if (replyEdit) {
        replyEdit->SetColors(text, bg);
    }
    HwndScheduleRepaint(hwnd);
}

void CommentCards::UpdateFont(PlatformFont* f) {
    font = f;
    lineDy = 0;
    if (replyEdit) {
        replyEdit->SetFont(f);
    }
    Relayout();
}

//--- painting

static void DrawChevron(Gfx* gfx, Rect r, bool collapsed, Color col) {
    int cx = r.x + r.dx / 2;
    int cy = r.y + r.dy / 2;
    int h = r.dx / 2;
    if (collapsed) {
        // >
        gfx->DrawLineAA({cx - h / 2, cy - h}, {cx + h / 2, cy}, col, 1.5f);
        gfx->DrawLineAA({cx + h / 2, cy}, {cx - h / 2, cy + h}, col, 1.5f);
        return;
    }
    // v
    gfx->DrawLineAA({cx - h, cy - h / 2}, {cx, cy + h / 2}, col, 1.5f);
    gfx->DrawLineAA({cx, cy + h / 2}, {cx + h, cy - h / 2}, col, 1.5f);
}

static void DrawLine(HDC hdc, Str s, Rect r, uint format, HFONT font, Color col) {
    SetTextColor(hdc, col);
    HdcDrawText(hdc, s, r, kLineFormat | format, font);
}

static void PaintGroup(CommentCards* w, HDC hdc, Gfx* gfx, CommentCardGroup* g, int offY) {
    Rect r = g->rect;
    r.y -= offY;
    Color fill = AccentColor(w->bgCol, 18, 30);
    gfx->FillRoundedRect(r, Scale(w, kRadius), fill);
    int pad = Scale(w, kPad);
    int chevronDx = Scale(w, kChevronDx);
    Rect chevron{r.x + r.dx - pad - chevronDx, r.y, chevronDx, r.dy};
    DrawChevron(gfx, chevron, g->collapsed, w->textCol);

    TempStr count = fmt("%d", len(g->cards));
    int countDx = TextDx(hdc, count, w->font->GetHFont());
    Rect countR{chevron.x - pad - countDx, r.y, countDx, r.dy};
    DrawLine(hdc, count, countR, 0, w->font->GetHFont(), w->textCol);
    Rect titleR{r.x + pad, r.y, countR.x - r.x - 2 * pad, r.dy};
    DrawLine(hdc, g->title, titleR, 0, BoldHFont(w), w->textCol);
}

static int PaintText(HDC hdc, Str s, Rect r, HFONT font, Color col) {
    int dy = TextDy(hdc, s, r.dx, font);
    r.dy = dy;
    SetTextColor(hdc, col);
    HdcDrawText(hdc, s, r, kTextFormat | DT_END_ELLIPSIS, font);
    return dy;
}

static void PaintCard(CommentCards* w, HDC hdc, Gfx* gfx, CommentCard* c, int offY) {
    bool isSel = c == w->selected;
    bool focused = HwndIsFocused(w->hwnd) || (w->replyEdit && HwndIsFocused(w->replyEdit->hwnd));
    Rect r = c->rect;
    r.y -= offY;
    Color fill = isSel ? AccentColor(w->bgCol, 10, 18) : w->bgCol;
    Color border = AccentColor(w->bgCol, 40, 60);
    if (isSel && focused) {
        border = AccentColor(w->bgCol, 110, 120);
    }
    gfx->FillRoundedRect(r, Scale(w, kRadius), fill, border);

    HFONT font = w->font->GetHFont();
    HFONT bold = BoldHFont(w);
    Color muted = MutedCol(w);
    int pad = Scale(w, kPad);
    int gap = Scale(w, kLineGap);
    int lineDy = LineDy(w);
    int x = r.x + pad;
    int dx = r.dx - 2 * pad;
    int y = r.y + pad;

    // [icon] author  kind ... (o)
    Pixmap* icon = GetCachedPixmapForSvg(Str(gIconSidebarComments), lineDy, lineDy, w->textCol);
    gfx->DrawPixmap(icon, {x, y, lineDy, lineDy});
    int textX = x + lineDy + Scale(w, 4);
    int right = x + dx;
    if (!ColorSkipsPaint(c->swatch)) {
        int sw = Scale(w, kSwatchDx);
        Rect sr{right - sw, y + (lineDy - sw) / 2, sw, sw};
        gfx->FillEllipse(sr, c->swatch);
        right -= sw + Scale(w, 4);
    }
    Str author = len(c->author) > 0 ? c->author : Str(Tr("(no author)"));
    int authorDx = TextDx(hdc, author, bold);
    authorDx = std::min(authorDx, right - textX);
    DrawLine(hdc, author, {textX, y, authorDx, lineDy}, 0, bold, w->textCol);
    int kindX = textX + authorDx + Scale(w, 6);
    if (kindX < right) {
        DrawLine(hdc, c->kind, {kindX, y, right - kindX, lineDy}, 0, font, muted);
    }
    y += lineDy;

    if (len(c->text) > 0) {
        y += gap;
        y += PaintText(hdc, c->text, {x, y, dx, 0}, font, w->textCol);
    }

    // replies, indented behind a rule
    int indent = Scale(w, kReplyIndent);
    for (CommentCardReply* rep : c->replies) {
        y += gap * 2;
        int top = y;
        Str ra = len(rep->author) > 0 ? rep->author : Str(Tr("(no author)"));
        int raDx = TextDx(hdc, ra, bold);
        raDx = std::min(raDx, dx - indent);
        DrawLine(hdc, ra, {x + indent, y, raDx, lineDy}, 0, bold, w->textCol);
        int dateX = x + indent + raDx + Scale(w, 6);
        if (dateX < x + dx) {
            DrawLine(hdc, rep->date, {dateX, y, x + dx - dateX, lineDy}, 0, font, muted);
        }
        y += lineDy;
        if (len(rep->text) > 0) {
            y += gap;
            y += PaintText(hdc, rep->text, {x + indent, y, dx - indent, 0}, font, w->textCol);
        }
        gfx->FillRect({x + Scale(w, 2), top, Scale(w, 2), y - top}, AccentColor(w->bgCol, 50, 70));
    }

    // status ... date
    y += gap * 2;
    int dateDx = TextDx(hdc, c->date, font);
    DrawLine(hdc, c->date, {x + dx - dateDx, y, dateDx, lineDy}, 0, font, muted);
    if (len(c->status) > 0) {
        TempStr s = str::JoinTemp(StatusMark(c->statusKind), c->status);
        int stDx = x + dx - dateDx - Scale(w, 6) - x;
        DrawLine(hdc, s, {x, y, stDx, lineDy}, 0, bold, StatusCol(w, c->statusKind));
    }

    if (!c->replyBox.IsEmpty()) {
        Rect box = c->replyBox;
        box.y -= offY;
        gfx->FillRect(box, w->bgCol);
        gfx->DrawRect(box, border);
    }
}

void CommentCards::Paint(HDC hdcWin) {
    Rect rc = HwndClientRect(hwnd);
    DoubleBuffer buffer(hwnd, rc);
    HDC hdc = buffer.GetDC();
    HdcFillRect(hdc, rc, AccentColor(bgCol, 6, 10));
    SetBkMode(hdc, TRANSPARENT);
    GfxHdc gfx(hdc);

    if (len(groups) == 0) {
        Rect r = rc;
        r.y += Scale(this, kMargin * 2);
        r.dy = LineDy(this);
        SetTextColor(hdc, MutedCol(this));
        HdcDrawText(hdc, Tr("No comments"), r, DT_CENTER | kLineFormat, font->GetHFont());
    }
    for (CommentCardGroup* g : groups) {
        Rect gr = g->rect;
        gr.y -= scrollY;
        if (gr.y > rc.dy) {
            break;
        }
        if (gr.y + gr.dy >= 0) {
            PaintGroup(this, hdc, &gfx, g, scrollY);
        }
        if (g->collapsed) {
            continue;
        }
        for (CommentCard* c : g->cards) {
            Rect cr = c->rect;
            cr.y -= scrollY;
            if (cr.y > rc.dy) {
                break;
            }
            if (cr.y + cr.dy >= 0) {
                PaintCard(this, hdc, &gfx, c, scrollY);
            }
        }
    }
    buffer.Flush(hdcWin);
}

//--- input

// pt in client coords
CommentCard* CommentCards::CardAt(Point pt) {
    pt.y += scrollY;
    for (CommentCardGroup* g : groups) {
        if (g->collapsed) {
            continue;
        }
        for (CommentCard* c : g->cards) {
            if (c->rect.Contains(pt)) {
                return c;
            }
        }
    }
    return nullptr;
}

CommentCardGroup* CommentCards::GroupHeaderAt(Point pt) {
    pt.y += scrollY;
    for (CommentCardGroup* g : groups) {
        if (g->rect.Contains(pt)) {
            return g;
        }
    }
    return nullptr;
}

void CommentCards::Notify(const CommentCardsHandler& h, CommentCard* c, CommentCardGroup* g) {
    CommentCardsEvent ev;
    ev.w = this;
    ev.card = c;
    ev.group = g;
    h.Call(&ev);
}

void CommentCards::ToggleGroup(CommentCardGroup* g) {
    g->collapsed = !g->collapsed;
    Relayout();
    Notify(onGroupToggled, nullptr, g);
}

void CommentCards::OnClick(Point pt, bool dblClick) {
    if (CommentCardGroup* g = GroupHeaderAt(pt)) {
        ToggleGroup(g);
        return;
    }
    CommentCard* c = CardAt(pt);
    if (!c) {
        return;
    }
    Point ptContent{pt.x, pt.y + scrollY};
    bool onReplyBox = c == selected && c->replyBox.Contains(ptContent);
    Select(c, true);
    if (onReplyBox) {
        FocusReply();
        return;
    }
    Notify(dblClick ? onActivated : onSelected, c, nullptr);
}

// the visible cards, in order
static void VisibleCards(CommentCards* w, Vec<CommentCard*>& out) {
    for (CommentCardGroup* g : w->groups) {
        if (g->collapsed) {
            continue;
        }
        for (CommentCard* c : g->cards) {
            VecAppend(out, c);
        }
    }
}

void CommentCards::SelectNext(int dir) {
    Vec<CommentCard*> cards;
    VisibleCards(this, cards);
    if (len(cards) == 0) {
        return;
    }
    int idx = VecFind(cards, selected);
    if (idx < 0) {
        idx = dir > 0 ? 0 : len(cards) - 1;
    } else {
        idx = std::clamp(idx + dir, 0, len(cards) - 1);
    }
    if (cards[idx] == selected) {
        return;
    }
    Select(cards[idx], true);
    Notify(onSelected, selected, nullptr);
}

// Up / Down pick a card, Left / Right fold its group, Enter opens it
bool CommentCards::OnKey(WPARAM key) {
    Rect rc = HwndClientRect(hwnd);
    switch (key) {
        case VK_UP:
            SelectNext(-1);
            return true;
        case VK_DOWN:
            SelectNext(1);
            return true;
        case VK_HOME:
            SelectNext(-len(groups) * 10000);
            return true;
        case VK_END:
            SelectNext(len(groups) * 10000);
            return true;
        case VK_PRIOR:
            Scroll(scrollY - rc.dy);
            return true;
        case VK_NEXT:
            Scroll(scrollY + rc.dy);
            return true;
        case VK_LEFT:
        case VK_RIGHT: {
            CommentCardGroup* g = selected ? GroupOf(selected) : nullptr;
            if (g && g->collapsed != (key == VK_LEFT)) {
                ToggleGroup(g);
            }
            return true;
        }
        case VK_RETURN:
            if (selected) {
                Notify(onActivated, selected, nullptr);
            }
            return true;
        case VK_DELETE:
            if (selected) {
                Notify(onDelete, selected, nullptr);
            }
            return true;
    }
    return false;
}

// Enter sends the reply, Esc drops it
static LRESULT CALLBACK WndProcReplyEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto* w = (CommentCards*)data;
    if (msg == WM_GETDLGCODE) {
        return DLGC_WANTALLKEYS | DefSubclassProc(hwnd, msg, wp, lp);
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        w->replyEdit->SetText(StrL(""));
        HwndSetFocus(w->hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        TempStr text = w->replyEdit->GetTextTemp();
        if (str::IsEmptyOrWhiteSpace(text) || !w->selected) {
            return 0;
        }
        CommentCardsEvent ev;
        ev.w = w;
        ev.card = w->selected;
        ev.text = text;
        w->onReply.Call(&ev);
        return 0;
    }
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        HwndScheduleRepaint(w->hwnd);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HWND CommentCards::Create(const CreateArgs& args) {
    CreateCustomArgs cargs;
    cargs.parent = args.parent;
    cargs.style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | WS_CLIPCHILDREN;
    cargs.font = args.font;
    cargs.isRtl = args.isRtl;
    CreateCustom(cargs);
    if (!hwnd) {
        return nullptr;
    }
    shouldEraseBackground = false;
    SetFont(args.font);

    replyEdit = new Edit();
    Edit::CreateArgs eargs;
    eargs.parent = hwnd;
    eargs.cueText = Tr("Add Reply");
    eargs.font = args.font;
    eargs.isRtl = args.isRtl;
    eargs.centerTextVert = true;
    replyEdit->Create(eargs);
    ShowWindow(replyEdit->hwnd, SW_HIDE);
    SetWindowSubclass(replyEdit->hwnd, WndProcReplyEdit, NextSubclassId(), (DWORD_PTR)this);
    return hwnd;
}

Size CommentCards::GetIdealSize() {
    return {Scale(this, 120), Scale(this, 120)};
}

static void OnVScroll(CommentCards* w, WPARAM wp) {
    Rect rc = HwndClientRect(w->hwnd);
    int lineDy = LineDy(w);
    int y = w->scrollY;
    switch (LOWORD(wp)) {
        case SB_LINEUP:
            y -= lineDy;
            break;
        case SB_LINEDOWN:
            y += lineDy;
            break;
        case SB_PAGEUP:
            y -= rc.dy;
            break;
        case SB_PAGEDOWN:
            y += rc.dy;
            break;
        case SB_TOP:
            y = 0;
            break;
        case SB_BOTTOM:
            y = w->contentDy;
            break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            GetScrollInfo(w->hwnd, SB_VERT, &si);
            y = si.nTrackPos;
            break;
        }
    }
    w->Scroll(y);
}

LRESULT CommentCards::OnMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    LRESULT res = TryReflectMessages(hwnd, msg, wp, lp);
    if (res) {
        return res;
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            Relayout();
            return 0;
        case WM_VSCROLL:
            OnVScroll(this, wp);
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            Scroll(scrollY - MulDiv(delta, LineDy(this) * 3, WHEEL_DELTA));
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            HwndSetFocus(hwnd);
            OnClick({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}, msg == WM_LBUTTONDBLCLK);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            HwndSetFocus(hwnd);
            CommentCard* c = CardAt({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (c) {
                Select(c, false);
                Notify(onSelected, c, nullptr);
            }
            return 0;
        }
        case WM_CONTEXTMENU: {
            Point pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            CommentCard* c = selected;
            if (pt.x == -1 && pt.y == -1) {
                // from the keyboard: at the selected card
                if (!c) {
                    return 0;
                }
                POINT p{c->rect.x + Scale(this, kPad), c->rect.y - scrollY + Scale(this, kPad)};
                ClientToScreen(hwnd, &p);
                pt = {p.x, p.y};
            } else {
                POINT p{pt.x, pt.y};
                ScreenToClient(hwnd, &p);
                c = CardAt({p.x, p.y});
            }
            if (!c) {
                return 0;
            }
            CommentCardsEvent ev;
            ev.w = this;
            ev.card = c;
            ev.pt = pt;
            onContextMenu.Call(&ev);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            if (OnKey(wp)) {
                return 0;
            }
            break;
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            HwndScheduleRepaint(hwnd);
            break;
    }
    return ControlBase::OnMessage(hwnd, msg, wp, lp);
}

// keys the focused cards act on, not the app's shortcuts
bool CommentCardsTakeKey(HWND hwnd, WPARAM key) {
    ControlBase* c = ControlFromHwnd(hwnd);
    if (!c || c->kind != kindCommentCards || IsAltPressed() || IsCtrlPressed()) {
        return false;
    }
    switch (key) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RETURN:
        case VK_DELETE:
            return true;
    }
    return false;
}
