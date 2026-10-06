/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/UITask.h"
#include "base/Win.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/GuiColors.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "AppSettings.h"
#include "Annotation.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Commands.h"
#include "Translations.h"
#include "Menu.h"
#include "Theme.h"
#include "Toolbar.h"
#include "AnnotSearch.h"
#include "AnnotEditToolbar.h"
#include "AnnotTextPopup.h"
#include "SidebarPanel.h"
#include "CommentCards.h"

#include "CommentsPanel.h"

/*
The Comments sidebar view: the document's annotations as cards, grouped by
page (or author, date, ...), with replies and review status in the card.

  [search comments     ] [All authors v]
  [By page                            v]
  [ Page 3                         1 v ]
  | alice  Highlight                   |
  | check this value                   |
  |  | bob: agreed                     |
  | v Completed (bob)       2026-09-12 |
*/

constexpr int kAuthorListDx = 120;
// ids of the Set Status submenu items; only seen by our TrackPopupMenu
constexpr int kCmdSetStatusFirst = CmdFirstCustom + 9000;

enum class CommentsGroupBy {
    Page,
    Author,
    Date,
    Type,
    Status,
    Color,
};

// untranslated, for tests; same order as CommentsGroupBy
static SeqStrings gGroupByNames = "page\0author\0date\0type\0status\0color\0";

struct CommentsPanel {
    MainWindow* win = nullptr;
    // owns the controls
    VBox* layout = nullptr;
    Edit* filterEdit = nullptr;
    DropDown* authorList = nullptr;
    DropDown* groupList = nullptr;
    CommentCards* cards = nullptr;
    // the author list's entries after "All authors"
    StrVec authors;
    AnnotMatchOpts filter;
    CommentsGroupBy groupBy = CommentsGroupBy::Page;
    // "<groupBy>:<title>" of what the user collapsed, kept across rebuilds
    StrVec collapsedGroups;
    bool loaded = false;
    bool rebuildPosted = false;
};

static EngineBase* TabEngine(WindowTab* tab) {
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    return dm ? dm->GetEngine() : nullptr;
}

bool CanShowComments(WindowTab* tab) {
    EngineBase* engine = TabEngine(tab);
    return engine && EngineSupportsAnnotations(engine);
}

// what a reader added: not links, form fields or popups
static bool IsListedAnnot(Annotation* a) {
    if (a->isReply) {
        return false;
    }
    switch (a->type) {
        case AnnotationType::Link:
        case AnnotationType::Popup:
        case AnnotationType::Widget:
            return false;
        default:
            return true;
    }
}

// first line of the contents, for test dumps
static TempStr ContentsLineTemp(Annotation* a) {
    Str s = Contents(a);
    int n = 0;
    while (n < len(s) && s.s[n] != '\n' && s.s[n] != '\r') {
        n++;
    }
    return str::DupTemp(Str(s.s, n));
}

// "Highlight (alice): check this"; a reply: "bob: agreed"
static TempStr CommentLabelTemp(Annotation* a) {
    TempStr author = str::DupTemp(Author(a));
    TempStr line = ContentsLineTemp(a);
    if (a->isReply) {
        if (len(author) == 0) {
            return line;
        }
        return len(line) == 0 ? author : fmt("%s: %s", author, line);
    }
    Str type = AnnotationReadableNameTemp(a->type);
    TempStr head = len(author) > 0 ? fmt("%s (%s)", type, author) : str::DupTemp(type);
    return len(line) == 0 ? head : fmt("%s: %s", head, line);
}

static TempStr TimeTemp(Annotation* a, const char* format) {
    time_t secs = ModificationDate(a);
    struct tm tm;
    if (secs == 0 || localtime_s(&tm, &secs) != 0) {
        return {};
    }
    char buf[64];
    size_t n = strftime(buf, sizeof buf, format, &tm);
    return str::DupTemp(Str(buf, (int)n));
}

// local time, like the comment card
static TempStr DateTemp(Annotation* a) {
    return TimeTemp(a, "%Y-%m-%d %H:%M");
}

static Str ReviewStateLabel(ReviewState st) {
    switch (st) {
        case ReviewState::Accepted:
            return Tr("Accepted");
        case ReviewState::Rejected:
            return Tr("Rejected");
        case ReviewState::Cancelled:
            return Tr("Cancelled");
        case ReviewState::Completed:
            return Tr("Completed");
        default:
            return Tr("None");
    }
}

// where the thread is, None when nobody set a state
static ReviewState ThreadState(Annotation* a, Annotation** byOut = nullptr) {
    Annotation* sr = ReviewStateReply(a);
    if (byOut) {
        *byOut = sr;
    }
    return sr ? ReviewStateOf(sr) : ReviewState::None;
}

static CommentCardStatus CardStatus(ReviewState st) {
    switch (st) {
        case ReviewState::Accepted:
        case ReviewState::Completed:
            return CommentCardStatus::Done;
        case ReviewState::Rejected:
        case ReviewState::Cancelled:
            return CommentCardStatus::Declined;
        default:
            return CommentCardStatus::None;
    }
}

// "Completed (bob)"; empty with no state
static TempStr StatusTemp(Annotation* a) {
    Annotation* by = nullptr;
    ReviewState st = ThreadState(a, &by);
    if (st == ReviewState::None) {
        return {};
    }
    Str label = ReviewStateLabel(st);
    Str author = Author(by);
    return len(author) > 0 ? fmt("%s (%s)", label, author) : str::DupTemp(label);
}

static Color SwatchColor(Annotation* a) {
    PdfColor c = GetColor(a);
    if (c == kColorUnset) {
        return kColorUnset;
    }
    u8 r, g, b, alpha;
    UnpackPdfColor(c, r, g, b, alpha);
    return alpha == 0 ? kColorUnset : MkRgb(r, g, b);
}

static Str SelectedAuthor(CommentsPanel* cp) {
    // 0 is "All authors"
    int idx = CbGetCurrentSelection(cp->authorList);
    if (idx <= 0 || idx > len(cp->authors)) {
        return {};
    }
    return cp->authors[idx - 1];
}

static bool CommentMatches(CommentsPanel* cp, Annotation* a, Str author) {
    if (len(author) > 0 && !str::EqI(Author(a), author)) {
        return false;
    }
    return AnnotMatches(a, cp->filter);
}

static bool SameStrVec(const StrVec& a, const StrVec& b) {
    if (len(a) != len(b)) {
        return false;
    }
    for (int i = 0; i < len(a); i++) {
        if (!str::Eq(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

// every author in the document, keeping the chosen one chosen
static void UpdateAuthorList(CommentsPanel* cp, const Vec<Annotation*>& annots) {
    StrVec authors;
    for (Annotation* a : annots) {
        // who set a status isn't an author of comments
        if (a->isState) {
            continue;
        }
        Str author = Author(a);
        if (len(author) > 0 && authors.FindI(author) < 0) {
            authors.Append(author);
        }
    }
    SortNoCase(&authors);
    if (SameStrVec(authors, cp->authors)) {
        return;
    }
    TempStr sel = str::DupTemp(SelectedAuthor(cp));
    cp->authors.Reset();
    StrVec items;
    items.Append(Tr("All authors"));
    for (Str s : authors) {
        cp->authors.Append(s);
        items.Append(s);
    }
    cp->authorList->SetItems(items);
    int idx = len(sel) > 0 ? cp->authors.FindI(sel) : -1;
    CbSetCurrentSelection(cp->authorList, idx + 1);
}

//--- grouping

// the group a comment goes in, and where that group sorts
struct GroupKey {
    TempStr title;
    // groups sort by this, then by title
    i64 order = 0;
};

static GroupKey GroupKeyOf(CommentsPanel* cp, Annotation* a) {
    GroupKey k;
    switch (cp->groupBy) {
        case CommentsGroupBy::Page:
            k.title = fmt(Tr("Page %d").s, a->pageNo);
            k.order = a->pageNo;
            break;
        case CommentsGroupBy::Author: {
            Str author = Author(a);
            k.title = len(author) > 0 ? str::DupTemp(author) : str::DupTemp(Tr("(no author)"));
            // no author last
            k.order = len(author) > 0 ? 0 : 1;
            break;
        }
        case CommentsGroupBy::Date:
            k.title = TimeTemp(a, "%Y-%m-%d");
            if (len(k.title) == 0) {
                k.title = str::DupTemp(Tr("No date"));
            }
            // newest first
            k.order = -(i64)ModificationDate(a) / (24 * 3600);
            break;
        case CommentsGroupBy::Type:
            k.title = str::DupTemp(AnnotationReadableNameTemp(a->type));
            break;
        case CommentsGroupBy::Status: {
            ReviewState st = ThreadState(a);
            k.title = str::DupTemp(st == ReviewState::None ? Tr("No status") : ReviewStateLabel(st));
            k.order = (int)st;
            break;
        }
        case CommentsGroupBy::Color: {
            Color c = SwatchColor(a);
            if (c == kColorUnset) {
                k.title = str::DupTemp(Tr("No color"));
                k.order = 1;
            } else {
                k.title = fmt("#%02x%02x%02x", (int)GetRValue(c), (int)GetGValue(c), (int)GetBValue(c));
            }
            break;
        }
    }
    return k;
}

static TempStr CollapseKeyTemp(CommentsPanel* cp, Str title) {
    return fmt("%d:%s", (int)cp->groupBy, title);
}

struct GroupEntry {
    CommentCardGroup* group = nullptr;
    i64 order = 0;
};

static bool GroupLess(const GroupEntry& a, const GroupEntry& b) {
    if (a.order != b.order) {
        return a.order < b.order;
    }
    return str::CmpI(a.group->title, b.group->title) < 0;
}

static CommentCardGroup* FindOrAddGroup(CommentsPanel* cp, Vec<GroupEntry>& groups, const GroupKey& k) {
    for (GroupEntry& e : groups) {
        if (str::Eq(e.group->title, k.title)) {
            return e.group;
        }
    }
    auto* g = new CommentCardGroup();
    g->title = str::Dup(k.title);
    g->collapsed = cp->collapsedGroups.Find(CollapseKeyTemp(cp, k.title)) >= 0;
    VecAppend(groups, GroupEntry{g, k.order});
    return g;
}

static TempStr CardTextTemp(Annotation* a) {
    TempStr s = str::DupTemp(Contents(a));
    str::NormalizeNewlinesToLFInPlace(s);
    return s;
}

static CommentCard* MakeCard(CommentsPanel* cp, Annotation* a, const Vec<Annotation*>& replies) {
    auto* c = new CommentCard();
    c->data = a;
    c->author = str::Dup(Author(a));
    Str type = AnnotationReadableNameTemp(a->type);
    if (cp->groupBy == CommentsGroupBy::Page) {
        c->kind = str::Dup(type);
    } else {
        c->kind = str::Dup(fmt(Tr("%s, page %d").s, type, a->pageNo));
    }
    c->date = str::Dup(DateTemp(a));
    c->text = str::Dup(CardTextTemp(a));
    c->status = str::Dup(StatusTemp(a));
    c->statusKind = CardStatus(ThreadState(a));
    c->swatch = SwatchColor(a);
    c->canReply = CanAccessDisk() && CanReplyToAnnotation(a);
    for (Annotation* r : replies) {
        auto* cr = new CommentCardReply();
        cr->author = str::Dup(Author(r));
        cr->date = str::Dup(DateTemp(r));
        cr->text = str::Dup(CardTextTemp(r));
        VecAppend(c->replies, cr);
    }
    return c;
}

// newest first within a date group; otherwise in document order
static void SortCardsByDate(CommentCardGroup* g) {
    Vec<CommentCard*>& v = g->cards;
    for (int i = 1; i < len(v); i++) {
        CommentCard* n = v[i];
        time_t t = ModificationDate((Annotation*)n->data);
        int j = i - 1;
        for (; j >= 0 && ModificationDate((Annotation*)v[j]->data) < t; j--) {
            v[j + 1] = v[j];
        }
        v[j + 1] = n;
    }
}

static void SortGroups(Vec<GroupEntry>& v) {
    for (int i = 1; i < len(v); i++) {
        GroupEntry n = v[i];
        int j = i - 1;
        for (; j >= 0 && GroupLess(n, v[j]); j--) {
            v[j + 1] = v[j];
        }
        v[j + 1] = n;
    }
}

static CommentCard* FindCard(CommentsPanel* cp, Annotation* a) {
    for (CommentCardGroup* g : cp->cards->groups) {
        for (CommentCard* c : g->cards) {
            if (c->data == a) {
                return c;
            }
        }
    }
    return nullptr;
}

static void SyncSelection(CommentsPanel* cp) {
    WindowTab* tab = cp->win->CurrentTab();
    Annotation* sel = tab ? tab->selectedAnnotation : nullptr;
    if (!sel) {
        return;
    }
    CommentCard* cur = cp->cards->selected;
    if (cur && cur->data == sel) {
        return;
    }
    if (CommentCard* c = FindCard(cp, sel)) {
        cp->cards->Select(c, true);
    }
}

// threads where the comment or a reply matches the filter, in groups
static void BuildCards(CommentsPanel* cp) {
    WindowTab* tab = cp->win->CurrentTab();
    EngineBase* engine = CanShowComments(tab) ? TabEngine(tab) : nullptr;
    Vec<Annotation*> annots;
    if (engine) {
        EngineMupdfGetLoadedAnnotations(engine, annots);
    }
    UpdateAuthorList(cp, annots);
    Str author = SelectedAuthor(cp);

    // keep the selected card selected, and the view where it was
    CommentCard* prevSel = cp->cards->selected;
    Annotation* keepSel = prevSel ? (Annotation*)prevSel->data : nullptr;
    int keepScrollY = cp->cards->scrollY;

    Vec<GroupEntry> groups;
    int nComments = 0;
    for (Annotation* a : annots) {
        if (!IsListedAnnot(a)) {
            continue;
        }
        nComments++;
        Vec<Annotation*> replies;
        GetCommentReplies(a, replies);
        bool show = CommentMatches(cp, a, author);
        for (int i = 0; !show && i < len(replies); i++) {
            show = CommentMatches(cp, replies[i], author);
        }
        if (!show) {
            continue;
        }
        CommentCardGroup* g = FindOrAddGroup(cp, groups, GroupKeyOf(cp, a));
        VecAppend(g->cards, MakeCard(cp, a, replies));
    }
    SortGroups(groups);
    Vec<CommentCardGroup*> sorted;
    for (GroupEntry& e : groups) {
        if (cp->groupBy == CommentsGroupBy::Date) {
            SortCardsByDate(e.group);
        }
        VecAppend(sorted, e.group);
    }

    cp->cards->SetGroups(sorted);
    if (keepSel && VecContains(annots, keepSel)) {
        cp->cards->Select(FindCard(cp, keepSel), false);
    }
    cp->cards->Scroll(keepScrollY);
    cp->loaded = true;
    EditSetCueText(cp->filterEdit, fmt(Tr("Search %d comments").s, nComments));
    SyncSelection(cp);
}

static CommentsPanel* PanelOf(MainWindow* win) {
    return win ? win->commentsPanel : nullptr;
}

static bool IsShown(CommentsPanel* cp) {
    return IsSidebarViewShown(cp->win, SidebarView::Comments);
}

static void PostedRebuild(MainWindow* win) {
    if (!IsMainWindowValidAndNotClosing(win)) {
        return;
    }
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    cp->rebuildPosted = false;
    if (IsShown(cp)) {
        BuildCards(cp);
    }
}

// The annotations changed. Rebuilds once for a burst of changes (background
// loading reports every page)
void RefreshCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    cp->loaded = false;
    if (!IsShown(cp) || cp->rebuildPosted) {
        return;
    }
    cp->rebuildPosted = true;
    uitask::Post(MkFunc0(PostedRebuild, win), "RebuildComments");
}

// The cards' Annotation* belong to an engine about to go away
void ClearCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    Vec<CommentCardGroup*> none;
    cp->cards->SetGroups(none);
    cp->collapsedGroups.Reset();
    RefreshCommentsPanel(win);
}

// the panel was shown, or the tab changed under it
void UpdateCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp || cp->loaded || !IsShown(cp)) {
        return;
    }
    StartLoadingAnnotationsForUi(win->CurrentTab());
    BuildCards(cp);
}

void CommentsPanelSyncSelection(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (cp && IsShown(cp)) {
        SyncSelection(cp);
    }
}

// The annotation of a card, if it's still there: a click can come in before
// the rebuild after a delete
static Annotation* LiveAnnot(CommentsPanel* cp, CommentCard* c) {
    if (!c || !c->data) {
        return nullptr;
    }
    EngineBase* engine = TabEngine(cp->win->CurrentTab());
    Vec<Annotation*> annots;
    if (engine) {
        EngineMupdfGetLoadedAnnotations(engine, annots);
    }
    Annotation* a = (Annotation*)c->data;
    return VecContains(annots, a) ? a : nullptr;
}

// the page may be showing, the annotation further down it not
static void ScrollAnnotIntoView(MainWindow* win, DisplayModel* dm, Annotation* a) {
    int pageNo = PageNo(a);
    Rect canvas = HwndClientRect(win->hwndCanvas);
    Rect r = dm->CvtToScreen(pageNo, GetRect(a));
    if (dm->PageVisible(pageNo) && !canvas.Intersect(r).IsEmpty()) {
        return;
    }
    dm->GoToPage(pageNo, true);
    r = dm->CvtToScreen(pageNo, GetRect(a));
    if (canvas.Intersect(r).IsEmpty()) {
        // a quarter down from the top, not at the very edge
        dm->ScrollYBy(r.y - canvas.dy / 4, false);
    }
}

// goes to a card's comment and selects it
static Annotation* ChooseComment(CommentsPanel* cp, CommentCard* c) {
    Annotation* a = LiveAnnot(cp, c);
    WindowTab* tab = cp->win->CurrentTab();
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!a || !dm) {
        return nullptr;
    }
    ScrollAnnotIntoView(cp->win, dm, a);
    SetSelectedAnnotation(tab, a);
    return a;
}

static void ShowCard(CommentsPanel* cp, CommentCard* c, AnnotPopupFocus focus) {
    if (Annotation* a = ChooseComment(cp, c)) {
        ShowAnnotationTextPopup(cp->win, a, focus);
    }
}

// a comment takes its replies with it
static void DeleteComment(CommentsPanel* cp, CommentCard* c) {
    Annotation* a = LiveAnnot(cp, c);
    if (a) {
        DeleteAnnotationAndUpdateUI(cp->win->CurrentTab(), a);
    }
}

static void AfterThreadChanged(CommentsPanel* cp) {
    WindowTab* tab = cp->win->CurrentTab();
    RefreshAnnotationLists(tab);
    NotifyAnnotationsChanged(tab);
    ToolbarUpdateStateForWindow(cp->win, false);
}

static void SetStatus(CommentsPanel* cp, CommentCard* c, ReviewState st) {
    Annotation* a = LiveAnnot(cp, c);
    if (!a || !SetReviewState(a, st)) {
        return;
    }
    AfterThreadChanged(cp);
}

static void OnCardSelected(CommentsPanel* cp, CommentCardsEvent* ev) {
    ChooseComment(cp, ev->card);
}

static void OnCardActivated(CommentsPanel* cp, CommentCardsEvent* ev) {
    ShowCard(cp, ev->card, AnnotPopupFocus::Text);
}

static void OnCardDelete(CommentsPanel* cp, CommentCardsEvent* ev) {
    DeleteComment(cp, ev->card);
}

static void OnCardReply(CommentsPanel* cp, CommentCardsEvent* ev) {
    Annotation* a = LiveAnnot(cp, ev->card);
    TempStr text = str::DupTemp(ev->text);
    str::NormalizeNewlinesToLFInPlace(text);
    if (!a || !AddAnnotationReply(a, text)) {
        return;
    }
    cp->cards->replyEdit->SetText(StrL(""));
    AfterThreadChanged(cp);
}

static void OnGroupToggled(CommentsPanel* cp, CommentCardsEvent* ev) {
    TempStr key = CollapseKeyTemp(cp, ev->group->title);
    int idx = cp->collapsedGroups.Find(key);
    if (ev->group->collapsed && idx < 0) {
        cp->collapsedGroups.Append(key);
    } else if (!ev->group->collapsed && idx >= 0) {
        cp->collapsedGroups.RemoveAt(idx);
    }
}

// clang-format off
static MenuDef menuDefComments[] = {
    {
        TrN("Sho&w Comment"),
        CmdShowAnnotationText,
    },
    {
        TrN("&Reply to Comment"),
        CmdReplyToAnnotation,
    },
    {
        StrL(kMenuSeparator),
        0,
    },
    {
        TrN("Delete Annotation"),
        CmdDeleteAnnotation,
    },
    {
        {},
        0,
    },
};
// clang-format on

// None, Accepted, ... with the thread's state checked
static HMENU BuildStatusMenu(Annotation* a) {
    HMENU sub = CreatePopupMenu();
    ReviewState cur = ThreadState(a);
    const ReviewState states[] = {ReviewState::None, ReviewState::Accepted, ReviewState::Rejected,
                                  ReviewState::Cancelled, ReviewState::Completed};
    for (ReviewState st : states) {
        int id = kCmdSetStatusFirst + (int)st;
        AppendMenuW(sub, MF_STRING, id, ToWStrTemp(ReviewStateLabel(st)).s);
        MenuSetChecked(sub, id, st == cur);
    }
    return sub;
}

static void OnCardContextMenu(CommentsPanel* cp, CommentCardsEvent* ev) {
    CommentCard* c = ev->card;
    Annotation* a = LiveAnnot(cp, c);
    if (!a) {
        return;
    }
    HMENU popup = BuildMenuFromDef(menuDefComments, CreatePopupMenu(), nullptr);
    if (!AnnotationHasText(a)) {
        MenuRemove(popup, CmdShowAnnotationText);
    }
    bool canReply = CanAccessDisk() && CanReplyToAnnotation(a);
    if (!canReply) {
        MenuRemove(popup, CmdReplyToAnnotation);
    } else {
        // Set Status > before Delete
        HMENU sub = BuildStatusMenu(a);
        InsertMenuW(popup, CmdDeleteAnnotation, MF_BYCOMMAND | MF_POPUP | MF_STRING, (UINT_PTR)sub,
                    ToWStrTemp(Tr("Set &Status")).s);
        InsertMenuW(popup, CmdDeleteAnnotation, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
    }
    MarkMenuOwnerDraw(popup);
    Point pt = ev->pt;
    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, cp->win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);

    if (cmd >= kCmdSetStatusFirst && cmd <= kCmdSetStatusFirst + (int)ReviewState::Completed) {
        SetStatus(cp, c, (ReviewState)(cmd - kCmdSetStatusFirst));
        return;
    }
    switch (cmd) {
        case CmdShowAnnotationText:
            ShowCard(cp, c, AnnotPopupFocus::Text);
            break;
        case CmdReplyToAnnotation:
            cp->cards->FocusReply();
            break;
        case CmdDeleteAnnotation:
            DeleteComment(cp, c);
            break;
    }
}

// same syntax as the Annotations window (AnnotSearch.cpp)
static void OnFilterChanged(CommentsPanel* cp) {
    ParseAnnotFilterLenient(cp->filterEdit->GetTextTemp(), cp->filter);
    BuildCards(cp);
}

static void OnAuthorChanged(CommentsPanel* cp) {
    BuildCards(cp);
}

static void OnGroupByChanged(CommentsPanel* cp) {
    int idx = CbGetCurrentSelection(cp->groupList);
    cp->groupBy = (CommentsGroupBy)std::clamp(idx, 0, (int)CommentsGroupBy::Color);
    BuildCards(cp);
}

// Down goes into the cards, Esc clears the filter
static LRESULT CALLBACK WndProcFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    CommentsPanel* cp = (CommentsPanel*)data;
    if (msg == WM_KEYDOWN && wp == VK_DOWN) {
        HwndSetFocus(cp->cards->hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && len(cp->filterEdit->GetTextTemp()) > 0) {
        cp->filterEdit->SetText(StrL(""));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static DropDown* NewDropDown(HWND parent) {
    auto* dd = new DropDown();
    DropDown::CreateArgs args;
    args.parent = parent;
    args.font = GetAppFont();
    args.isRtl = IsUIRtl();
    dd->Create(args);
    return dd;
}

static void SetGroupByItems(CommentsPanel* cp) {
    StrVec items;
    items.Append(Tr("By page"));
    items.Append(Tr("By author"));
    items.Append(Tr("By date"));
    items.Append(Tr("By type"));
    items.Append(Tr("By status"));
    items.Append(Tr("By color"));
    cp->groupList->SetItems(items);
    CbSetCurrentSelection(cp->groupList, (int)cp->groupBy);
}

void CreateCommentsPanel(MainWindow* win) {
    auto* cp = new CommentsPanel();
    cp->win = win;
    HWND parent = win->sidebarTop->hwnd;
    int dpi = DpiGetForHwnd(parent);

    cp->filterEdit = new Edit();
    Edit::CreateArgs eargs;
    eargs.parent = parent;
    eargs.withBorder = true;
    eargs.cueText = Tr("Search Comments");
    eargs.font = GetAppFont();
    cp->filterEdit->Create(eargs);
    cp->filterEdit->onTextChanged = MkFunc0(OnFilterChanged, cp);
    SetWindowSubclass(cp->filterEdit->hwnd, WndProcFilterEdit, NextSubclassId(), (DWORD_PTR)cp);

    cp->authorList = NewDropDown(parent);
    cp->authorList->maxDx = DpiScaleByDpi(dpi, kAuthorListDx);
    cp->authorList->onSelectionChanged = MkFunc0(OnAuthorChanged, cp);
    StrVec items;
    items.Append(Tr("All authors"));
    cp->authorList->SetItems(items);
    CbSetCurrentSelection(cp->authorList, 0);

    cp->groupList = NewDropDown(parent);
    SetGroupByItems(cp);
    cp->groupList->onSelectionChanged = MkFunc0(OnGroupByChanged, cp);

    cp->cards = new CommentCards();
    CommentCards::CreateArgs cargs;
    cargs.parent = parent;
    cargs.font = GetAppFont();
    cargs.isRtl = IsUIRtl();
    cp->cards->onSelected = MkFunc1(OnCardSelected, cp);
    cp->cards->onActivated = MkFunc1(OnCardActivated, cp);
    cp->cards->onDelete = MkFunc1(OnCardDelete, cp);
    cp->cards->onReply = MkFunc1(OnCardReply, cp);
    cp->cards->onContextMenu = MkFunc1(OnCardContextMenu, cp);
    cp->cards->onGroupToggled = MkFunc1(OnGroupToggled, cp);
    cp->cards->Create(cargs);
    ReportIf(!cp->cards->hwnd);

    // [filter][author] and [group by] over the cards, which take the rest
    int gap = DpiScaleByDpi(dpi, 2);
    auto* header = new HBox();
    header->alignMain = MainAxisAlign::MainStart;
    header->alignCross = CrossAxisAlign::CrossCenter;
    header->gap = gap;
    header->AddChild(cp->filterEdit, 1);
    header->AddChild(cp->authorList);

    cp->layout = new VBox();
    cp->layout->alignMain = MainAxisAlign::MainStart;
    cp->layout->alignCross = CrossAxisAlign::Stretch;
    cp->layout->AddChild(header);
    cp->layout->AddChild(new Spacer(0, gap));
    cp->layout->AddChild(cp->groupList);
    cp->layout->AddChild(new Spacer(0, gap));
    cp->layout->AddChild(cp->cards, 1);

    win->commentsPanel = cp;
    UpdateCommentsPanelColors(win);
}

void DeleteCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    win->commentsPanel = nullptr;
    delete cp->layout;
    delete cp;
}

ILayout* CommentsViewLayout(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    return cp ? cp->layout : nullptr;
}

int CommentsViewControls(MainWindow* win, ControlBase* out[kMaxViewControls]) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return 0;
    }
    out[0] = cp->filterEdit;
    out[1] = cp->authorList;
    out[2] = cp->groupList;
    out[3] = cp->cards;
    return 4;
}

HWND CommentsFocusHwnd(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    return cp ? cp->cards->hwnd : nullptr;
}

void UpdateCommentsPanelColors(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    Color bgCol = ThemeControlBackgroundColor();
    Color txtCol = ThemeWindowTextColor();
    cp->cards->SetCardColors(txtCol, bgCol);
    cp->filterEdit->SetColors(txtCol, bgCol);
    cp->authorList->SetColors(txtCol, bgCol);
    cp->groupList->SetColors(txtCol, bgCol);
}

void UpdateCommentsPanelDpi(MainWindow* win, int dpi) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    PlatformFont* appFont = GetAppFontForDpi(dpi);
    cp->cards->UpdateFont(appFont);
    cp->filterEdit->SetFont(appFont);
    cp->authorList->SetFont(appFont);
    cp->authorList->maxDx = DpiScaleByDpi(dpi, kAuthorListDx);
    cp->groupList->SetFont(appFont);
}

//--- for tests

// a group, a card, or a reply in dump order
struct TestRow {
    CommentCardGroup* group = nullptr;
    CommentCard* card = nullptr;
};

static void CollectRows(CommentsPanel* cp, str::Builder& out, Vec<TestRow>& rows) {
    for (CommentCardGroup* g : cp->cards->groups) {
        Str collapsed = g->collapsed ? StrL(" collapsed") : Str{};
        out.Append(fmt("row %s (%d)%s\n", g->title, len(g->cards), collapsed));
        VecAppend(rows, TestRow{g, nullptr});
        if (g->collapsed) {
            continue;
        }
        for (CommentCard* c : g->cards) {
            auto* a = (Annotation*)c->data;
            out.Append(fmt("row   %s", CommentLabelTemp(a)));
            if (len(c->status) > 0) {
                out.Append(fmt(" [%s]", c->status));
            }
            out.AppendChar('\n');
            VecAppend(rows, TestRow{g, c});
            Vec<Annotation*> replies;
            GetCommentReplies(a, replies);
            for (Annotation* r : replies) {
                out.Append(fmt("row     %s\n", CommentLabelTemp(r)));
                VecAppend(rows, TestRow{g, c});
            }
        }
    }
}

// "3 Completed" -> "Completed"
static Str AfterSpace(Str s) {
    int i = str::IndexOfChar(s, ' ');
    return i < 0 ? Str{} : Str(s.s + i + 1, len(s) - i - 1);
}

// For tests: "filter" / "author" / "group" set the view, "choose" / "delete"
// act on the row with the given index (rows in dump order), "status" takes
// "<row> <State>", "toggle" folds the group of a row. Returns the cards.
TempStr CommentsPanelTestTemp(MainWindow* win, Str action, Str arg) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return fmt("FAIL no-panel");
    }
    str::Builder ignored;
    Vec<TestRow> rows;
    CollectRows(cp, ignored, rows);
    int idx = -1;
    str::Parse(arg, "%d", &idx);
    CommentCard* card = VecIsValidIndex(rows, idx) ? rows[idx].card : nullptr;
    if (str::Eq(action, StrL("filter"))) {
        cp->filterEdit->SetText(arg);
    } else if (str::Eq(action, StrL("author"))) {
        int i = len(arg) > 0 ? cp->authors.FindI(arg) : -1;
        CbSetCurrentSelection(cp->authorList, i + 1);
        BuildCards(cp);
    } else if (str::Eq(action, StrL("group"))) {
        int i = SeqStrIndex(gGroupByNames, arg);
        CbSetCurrentSelection(cp->groupList, std::max(i, 0));
        OnGroupByChanged(cp);
    } else if (str::Eq(action, StrL("choose"))) {
        if (card) {
            cp->cards->Select(card, true);
        }
        ChooseComment(cp, card);
    } else if (str::Eq(action, StrL("delete"))) {
        DeleteComment(cp, card);
    } else if (str::Eq(action, StrL("status"))) {
        int st = SeqStrIndex(gReviewStateNames, AfterSpace(arg));
        if (card && st >= 0) {
            SetStatus(cp, card, (ReviewState)st);
        }
    } else if (str::Eq(action, StrL("reply"))) {
        Str text = AfterSpace(arg);
        if (card && len(text) > 0) {
            cp->cards->Select(card, false);
            CommentCardsEvent ev;
            ev.card = card;
            ev.text = text;
            OnCardReply(cp, &ev);
        }
    } else if (str::Eq(action, StrL("toggle"))) {
        if (VecIsValidIndex(rows, idx)) {
            cp->cards->ToggleGroup(rows[idx].group);
        }
    } else if (str::Eq(action, StrL("rebuild"))) {
        BuildCards(cp);
    }

    WindowTab* tab = win->CurrentTab();
    Annotation* sel = tab ? tab->selectedAnnotation : nullptr;
    TempStr selLabel = AnnotationIsLive(sel) ? CommentLabelTemp(sel) : str::DupTemp(StrL("-"));
    str::Builder out;
    out.Append(fmt("comments shown=%d loaded=%d authors=%d author=%s group=%s selected=%s\n", IsShown(cp) ? 1 : 0,
                   cp->loaded ? 1 : 0, len(cp->authors), SelectedAuthor(cp),
                   SeqStrByIndex(gGroupByNames, (int)cp->groupBy), selLabel));
    VecReset(rows);
    CollectRows(cp, out, rows);
    return ToStrTemp(out);
}
