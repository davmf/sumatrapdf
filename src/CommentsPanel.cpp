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
#include "AnnotSearch.h"
#include "AnnotEditToolbar.h"
#include "AnnotTextPopup.h"
#include "SidebarPanel.h"

#include "CommentsPanel.h"

/*
The Comments sidebar view: the document's annotations grouped by page, with
replies under the comment they answer.

  [search comments     ] [All authors v]
  - Page 3 (1)
    - Highlight (alice): check this value
        bob: agreed
*/

constexpr int kMaxLabelLen = 80;
constexpr int kAuthorListDx = 120;

// a row of the tree: a page, a comment, or a reply
struct CommentNode {
    CommentNode* parent = nullptr;
    Vec<CommentNode*> kids;
    Str text;
    Str tip;
    // nullptr for a page (and the root)
    Annotation* annot = nullptr;
    int pageNo = 0;
    bool expanded = true;
    uintptr_t userData = 0;

    ~CommentNode() {
        str::Free(text);
        str::Free(tip);
        DeleteVecMembers(kids);
    }
};

static CommentNode* Node(TreeItem ti) {
    return (CommentNode*)ti;
}

struct CommentsTree : TreeModel {
    CommentNode* root = new CommentNode();

    ~CommentsTree() override { delete root; }
    TreeItem Root() override { return (TreeItem)root; }
    Str Text(TreeItem ti) override { return Node(ti)->text; }
    TreeItem Parent(TreeItem ti) override { return (TreeItem)Node(ti)->parent; }
    int ChildCount(TreeItem ti) override { return len(Node(ti)->kids); }
    TreeItem ChildAt(TreeItem ti, int idx) override { return (TreeItem)Node(ti)->kids[idx]; }
    bool IsExpanded(TreeItem ti) override { return Node(ti)->expanded; }
    bool IsChecked(TreeItem) override { return false; }
    void SetUserData(TreeItem ti, uintptr_t data) override { Node(ti)->userData = data; }
    uintptr_t GetUserData(TreeItem ti) override { return Node(ti)->userData; }
};

struct CommentsPanel {
    MainWindow* win = nullptr;
    // owns the controls
    VBox* layout = nullptr;
    Edit* filterEdit = nullptr;
    DropDown* authorList = nullptr;
    TreeView* tree = nullptr;
    CommentsTree* model = nullptr;
    // the author list's entries after "All authors"
    StrVec authors;
    AnnotMatchOpts filter;
    // what the user collapsed, kept across rebuilds; only compared, never read
    Vec<int> collapsedPages;
    Vec<Annotation*> collapsedThreads;
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

// first line of the contents, short enough for a row
static TempStr ContentsLineTemp(Annotation* a) {
    Str s = Contents(a);
    int n = 0;
    while (n < len(s) && s.s[n] != '\n' && s.s[n] != '\r') {
        n++;
    }
    if (n <= kMaxLabelLen) {
        return str::DupTemp(Str(s.s, n));
    }
    n = kMaxLabelLen;
    // don't cut a UTF-8 sequence in half
    while (n > 0 && ((u8)s.s[n] & 0xC0) == 0x80) {
        n--;
    }
    return str::JoinTemp(Str(s.s, n), StrL("..."));
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

// local time, like the comment card
static TempStr DateTemp(Annotation* a) {
    time_t secs = ModificationDate(a);
    struct tm tm;
    if (secs == 0 || localtime_s(&tm, &secs) != 0) {
        return {};
    }
    char buf[64];
    size_t n = strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return str::DupTemp(Str(buf, (int)n));
}

// "alice, 2026-09-12 10:30" over the whole contents
static TempStr CommentTipTemp(Annotation* a) {
    str::Builder sb;
    TempStr author = str::DupTemp(Author(a));
    TempStr date = DateTemp(a);
    sb.Append(author);
    if (len(date) > 0) {
        if (len(sb) > 0) {
            sb.Append(StrL(", "));
        }
        sb.Append(date);
    }
    Str contents = Contents(a);
    if (len(contents) > 0) {
        if (len(sb) > 0) {
            sb.AppendChar('\n');
        }
        sb.Append(contents);
    }
    return ToStrTemp(sb);
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

static bool TreeShowsModel(CommentsPanel* cp) {
    return cp->model && cp->tree->treeModel == cp->model;
}

static void SaveExpansion(CommentsPanel* cp) {
    if (!TreeShowsModel(cp)) {
        return;
    }
    for (CommentNode* page : cp->model->root->kids) {
        bool open = cp->tree->IsExpanded((TreeItem)page);
        VecRemove(cp->collapsedPages, page->pageNo);
        if (!open) {
            VecAppend(cp->collapsedPages, page->pageNo);
        }
        for (CommentNode* thread : page->kids) {
            if (len(thread->kids) == 0) {
                continue;
            }
            VecRemove(cp->collapsedThreads, thread->annot);
            if (!cp->tree->IsExpanded((TreeItem)thread)) {
                VecAppend(cp->collapsedThreads, thread->annot);
            }
        }
    }
}

static CommentNode* AddNode(CommentNode* parent, Annotation* a) {
    auto* n = new CommentNode();
    n->parent = parent;
    n->annot = a;
    n->pageNo = a->pageNo;
    n->text = str::Dup(CommentLabelTemp(a));
    n->tip = str::Dup(CommentTipTemp(a));
    VecAppend(parent->kids, n);
    return n;
}

static CommentNode* PageNode(CommentsPanel* cp, CommentNode* root, int pageNo) {
    for (int i = len(root->kids) - 1; i >= 0; i--) {
        if (root->kids[i]->pageNo == pageNo) {
            return root->kids[i];
        }
    }
    auto* n = new CommentNode();
    n->parent = root;
    n->pageNo = pageNo;
    n->expanded = !VecContains(cp->collapsedPages, pageNo);
    VecAppend(root->kids, n);
    return n;
}

// the annotations come in page order already; this keeps it if they don't
static void SortPages(CommentNode* root) {
    Vec<CommentNode*>& v = root->kids;
    for (int i = 1; i < len(v); i++) {
        CommentNode* n = v[i];
        int j = i - 1;
        for (; j >= 0 && v[j]->pageNo > n->pageNo; j--) {
            v[j + 1] = v[j];
        }
        v[j + 1] = n;
    }
}

static CommentNode* FindNode(CommentNode* n, Annotation* a) {
    if (n->annot == a) {
        return n;
    }
    for (CommentNode* kid : n->kids) {
        if (CommentNode* res = FindNode(kid, a)) {
            return res;
        }
    }
    return nullptr;
}

// a reply's thread is its top-level comment
static CommentNode* ThreadNode(CommentNode* n) {
    while (n && n->parent && n->parent->annot) {
        n = n->parent;
    }
    return n;
}

static void SyncSelection(CommentsPanel* cp) {
    WindowTab* tab = cp->win->CurrentTab();
    Annotation* sel = tab ? tab->selectedAnnotation : nullptr;
    if (!sel || !TreeShowsModel(cp)) {
        return;
    }
    CommentNode* cur = Node(cp->tree->GetSelection());
    if (cur && ThreadNode(cur)->annot == sel) {
        return;
    }
    if (CommentNode* n = FindNode(cp->model->root, sel)) {
        cp->tree->SelectItem((TreeItem)n);
    }
}

// threads where the comment or a reply matches the filter, by page
static void BuildTree(CommentsPanel* cp) {
    WindowTab* tab = cp->win->CurrentTab();
    EngineBase* engine = CanShowComments(tab) ? TabEngine(tab) : nullptr;
    SaveExpansion(cp);
    Vec<Annotation*> annots;
    if (engine) {
        EngineMupdfGetLoadedAnnotations(engine, annots);
    }
    UpdateAuthorList(cp, annots);
    Str author = SelectedAuthor(cp);

    auto* model = new CommentsTree();
    int nComments = 0;
    for (Annotation* a : annots) {
        if (!IsListedAnnot(a)) {
            continue;
        }
        nComments++;
        Vec<Annotation*> replies;
        GetAnnotationReplies(a, replies);
        bool show = CommentMatches(cp, a, author);
        for (int i = 0; !show && i < len(replies); i++) {
            show = CommentMatches(cp, replies[i], author);
        }
        if (!show) {
            continue;
        }
        CommentNode* page = PageNode(cp, model->root, a->pageNo);
        CommentNode* thread = AddNode(page, a);
        thread->expanded = !VecContains(cp->collapsedThreads, a);
        for (Annotation* r : replies) {
            AddNode(thread, r);
        }
    }
    SortPages(model->root);
    for (CommentNode* page : model->root->kids) {
        page->text = str::Dup(fmt(Tr("Page %d (%d)").s, page->pageNo, len(page->kids)));
    }

    CommentsTree* old = cp->model;
    cp->model = model;
    cp->tree->SetTreeModel(model);
    delete old;
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
        BuildTree(cp);
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

// The tree's Annotation* belong to an engine about to go away
void ClearCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    cp->tree->Clear();
    delete cp->model;
    cp->model = nullptr;
    VecReset(cp->collapsedPages);
    VecReset(cp->collapsedThreads);
    RefreshCommentsPanel(win);
}

// the panel was shown, or the tab changed under it
void UpdateCommentsPanel(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp || cp->loaded || !IsShown(cp)) {
        return;
    }
    StartLoadingAnnotationsForUi(win->CurrentTab());
    BuildTree(cp);
}

void CommentsPanelSyncSelection(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (cp && IsShown(cp)) {
        SyncSelection(cp);
    }
}

// The annotation of a row, if it's still there: a click can come in before
// the rebuild after a delete
static Annotation* LiveAnnot(CommentsPanel* cp, CommentNode* n) {
    if (!n || !n->annot || !TreeShowsModel(cp)) {
        return nullptr;
    }
    EngineBase* engine = TabEngine(cp->win->CurrentTab());
    Vec<Annotation*> annots;
    if (engine) {
        EngineMupdfGetLoadedAnnotations(engine, annots);
    }
    return VecContains(annots, n->annot) ? n->annot : nullptr;
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

// goes to a row's thread and selects it; replies aren't drawn, so not them
static Annotation* ChooseComment(CommentsPanel* cp, CommentNode* n) {
    Annotation* a = LiveAnnot(cp, ThreadNode(n));
    WindowTab* tab = cp->win->CurrentTab();
    DisplayModel* dm = tab ? tab->AsFixed() : nullptr;
    if (!a || !dm) {
        return nullptr;
    }
    ScrollAnnotIntoView(cp->win, dm, a);
    SetSelectedAnnotation(tab, a);
    return a;
}

static void ShowCard(CommentsPanel* cp, CommentNode* n, AnnotPopupFocus focus) {
    if (Annotation* a = ChooseComment(cp, n)) {
        ShowAnnotationTextPopup(cp->win, a, focus);
    }
}

// a comment takes its replies with it
static void DeleteComment(CommentsPanel* cp, CommentNode* n) {
    Annotation* a = LiveAnnot(cp, n);
    if (a) {
        DeleteAnnotationAndUpdateUI(cp->win->CurrentTab(), a);
    }
}

static CommentsPanel* PanelOfHwnd(HWND hwnd) {
    return PanelOf(FindMainWindowByHwnd(hwnd));
}

static void OnTreeSelectionChanged(TreeView::SelectionChangedEvent* ev) {
    // a click goes through OnTreeClick; programmatic changes go nowhere
    if (!ev->byKeyboard) {
        return;
    }
    if (CommentsPanel* cp = PanelOfHwnd(ev->treeView->hwnd)) {
        ChooseComment(cp, Node(ev->selectedItem));
    }
}

static void OnTreeClick(TreeView::ClickEvent* ev) {
    CommentsPanel* cp = PanelOfHwnd(ev->treeView->hwnd);
    CommentNode* n = Node(ev->treeItem);
    if (!cp || !n || !n->annot) {
        return;
    }
    if (!ev->isDblClick) {
        ChooseComment(cp, n);
        return;
    }
    ShowCard(cp, n, AnnotPopupFocus::Text);
    // not the default expand / collapse
    ev->result = 1;
}

static void OnTreeKeyDown(TreeView::KeyDownEvent* ev) {
    CommentsPanel* cp = PanelOfHwnd(ev->treeView->hwnd);
    if (!cp) {
        return;
    }
    CommentNode* n = Node(cp->tree->GetSelection());
    if (ev->keyCode == VK_RETURN) {
        ShowCard(cp, n, AnnotPopupFocus::Text);
        ev->result = 1;
        return;
    }
    if (ev->keyCode == VK_DELETE) {
        DeleteComment(cp, n);
        ev->result = 1;
    }
}

static void OnTreeGetTooltip(TreeView::GetTooltipEvent* ev) {
    CommentNode* n = Node(ev->treeItem);
    if (!n || len(n->tip) == 0) {
        return;
    }
    NMTVGETINFOTIPW* info = ev->info;
    str::BufSet(info->pszText, info->cchTextMax, n->tip);
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

static void OnTreeContextMenu(ContextMenuEvent* ev) {
    CommentsPanel* cp = PanelOfHwnd(ev->w->hwnd);
    if (!cp) {
        return;
    }
    Point pt{};
    CommentNode* n = Node(GetOrSelectTreeItemAtPos(ev, pt));
    if (!n || !n->annot) {
        return;
    }
    HMENU popup = BuildMenuFromDef(menuDefComments, CreatePopupMenu(), nullptr);
    Annotation* thread = ThreadNode(n)->annot;
    if (!AnnotationHasText(thread)) {
        MenuRemove(popup, CmdShowAnnotationText);
    }
    if (!CanAccessDisk() || !CanReplyToAnnotation(thread)) {
        MenuRemove(popup, CmdReplyToAnnotation);
    }
    MarkMenuOwnerDraw(popup);
    int cmd = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, cp->win->hwndFrame, nullptr);
    FreeMenuOwnerDrawInfoData(popup);
    DestroyMenu(popup);

    switch (cmd) {
        case CmdShowAnnotationText:
            ShowCard(cp, n, AnnotPopupFocus::Text);
            break;
        case CmdReplyToAnnotation:
            ShowCard(cp, n, AnnotPopupFocus::Reply);
            break;
        case CmdDeleteAnnotation:
            DeleteComment(cp, n);
            break;
    }
}

// same syntax as the Annotations window (AnnotSearch.cpp)
static void OnFilterChanged(CommentsPanel* cp) {
    ParseAnnotFilterLenient(cp->filterEdit->GetTextTemp(), cp->filter);
    BuildTree(cp);
}

static void OnAuthorChanged(CommentsPanel* cp) {
    BuildTree(cp);
}

// Down goes into the tree, Esc clears the filter
static LRESULT CALLBACK WndProcFilterEdit(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    CommentsPanel* cp = (CommentsPanel*)data;
    if (msg == WM_KEYDOWN && wp == VK_DOWN) {
        HwndSetFocus(cp->tree->hwnd);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && len(cp->filterEdit->GetTextTemp()) > 0) {
        cp->filterEdit->SetText(StrL(""));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
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

    cp->authorList = new DropDown();
    DropDown::CreateArgs dargs;
    dargs.parent = parent;
    dargs.font = GetAppFont();
    dargs.isRtl = IsUIRtl();
    cp->authorList->Create(dargs);
    cp->authorList->maxDx = DpiScaleByDpi(dpi, kAuthorListDx);
    cp->authorList->onSelectionChanged = MkFunc0(OnAuthorChanged, cp);
    StrVec items;
    items.Append(Tr("All authors"));
    cp->authorList->SetItems(items);
    CbSetCurrentSelection(cp->authorList, 0);

    cp->tree = new TreeView();
    TreeView::CreateArgs targs;
    targs.parent = parent;
    targs.font = GetAppTreeFont();
    targs.fullRowSelect = true;
    targs.isRtl = IsUIRtl();
    cp->tree->onSelectionChanged = MkFunc1Void(OnTreeSelectionChanged);
    cp->tree->onClick = MkFunc1Void(OnTreeClick);
    cp->tree->onKeyDown = MkFunc1Void(OnTreeKeyDown);
    cp->tree->onGetTooltip = MkFunc1Void(OnTreeGetTooltip);
    cp->tree->onContextMenu = MkFunc1Void(OnTreeContextMenu);
    cp->tree->Create(targs);
    ReportIf(!cp->tree->hwnd);

    // [filter][author] over the tree, which takes the remaining height
    auto* header = new HBox();
    header->alignMain = MainAxisAlign::MainStart;
    header->alignCross = CrossAxisAlign::CrossCenter;
    header->gap = DpiScaleByDpi(dpi, 2);
    header->AddChild(cp->filterEdit, 1);
    header->AddChild(cp->authorList);

    cp->layout = new VBox();
    cp->layout->alignMain = MainAxisAlign::MainStart;
    cp->layout->alignCross = CrossAxisAlign::Stretch;
    cp->layout->AddChild(header);
    cp->layout->AddChild(new Spacer(0, 2));
    cp->layout->AddChild(cp->tree, 1);

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
    delete cp->model;
    delete cp;
}

ILayout* CommentsViewLayout(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    return cp ? cp->layout : nullptr;
}

int CommentsViewControls(MainWindow* win, ControlBase* out[3]) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return 0;
    }
    out[0] = cp->filterEdit;
    out[1] = cp->authorList;
    out[2] = cp->tree;
    return 3;
}

HWND CommentsFocusHwnd(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    return cp ? cp->tree->hwnd : nullptr;
}

void UpdateCommentsPanelColors(MainWindow* win) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    Color bgCol = ThemeControlBackgroundColor();
    Color txtCol = ThemeWindowTextColor();
    cp->tree->SetColors(txtCol, bgCol);
    cp->filterEdit->SetColors(txtCol, bgCol);
    cp->authorList->SetColors(txtCol, bgCol);
}

void UpdateCommentsPanelDpi(MainWindow* win, int dpi) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return;
    }
    PlatformFont* appFont = GetAppFontForDpi(dpi);
    HwndSetTreeFontForDpi(cp->tree->hwnd, GetAppTreeFontForDpi(dpi)->GetHFont(), dpi);
    cp->filterEdit->SetFont(appFont);
    cp->authorList->SetFont(appFont);
    cp->authorList->maxDx = DpiScaleByDpi(dpi, kAuthorListDx);
}

static void DumpNode(str::Builder& out, CommentNode* n, int depth) {
    for (CommentNode* kid : n->kids) {
        out.Append(StrL("row "));
        for (int i = 0; i < depth; i++) {
            out.Append(StrL("  "));
        }
        out.Append(kid->text);
        out.AppendChar('\n');
        DumpNode(out, kid, depth + 1);
    }
}

static void CollectRows(CommentNode* n, Vec<CommentNode*>& out) {
    for (CommentNode* kid : n->kids) {
        VecAppend(out, kid);
        CollectRows(kid, out);
    }
}

// For tests: "filter" / "author" set the filters, "choose" / "delete" act on
// the row with the given index (rows in dump order). Returns the tree.
TempStr CommentsPanelTestTemp(MainWindow* win, Str action, Str arg) {
    CommentsPanel* cp = PanelOf(win);
    if (!cp) {
        return fmt("FAIL no-panel");
    }
    Vec<CommentNode*> rows;
    if (cp->model) {
        CollectRows(cp->model->root, rows);
    }
    int idx = -1;
    str::Parse(arg, "%d", &idx);
    CommentNode* row = VecIsValidIndex(rows, idx) ? rows[idx] : nullptr;
    if (str::Eq(action, StrL("filter"))) {
        cp->filterEdit->SetText(arg);
    } else if (str::Eq(action, StrL("author"))) {
        int i = len(arg) > 0 ? cp->authors.FindI(arg) : -1;
        CbSetCurrentSelection(cp->authorList, i + 1);
        BuildTree(cp);
    } else if (str::Eq(action, StrL("choose"))) {
        ChooseComment(cp, row);
    } else if (str::Eq(action, StrL("delete"))) {
        DeleteComment(cp, row);
    } else if (str::Eq(action, StrL("rebuild"))) {
        BuildTree(cp);
    }

    WindowTab* tab = win->CurrentTab();
    Annotation* sel = tab ? tab->selectedAnnotation : nullptr;
    TempStr selLabel = AnnotationIsLive(sel) ? CommentLabelTemp(sel) : str::DupTemp(StrL("-"));
    str::Builder out;
    out.Append(fmt("comments shown=%d loaded=%d authors=%d author=%s selected=%s\n", IsShown(cp) ? 1 : 0,
                   cp->loaded ? 1 : 0, len(cp->authors), SelectedAuthor(cp), selLabel));
    if (cp->model) {
        DumpNode(out, cp->model->root, 0);
    }
    return ToStrTemp(out);
}
