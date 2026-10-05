/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct CommentCards;

struct CommentCardReply {
    Str author;
    Str date;
    Str text;

    ~CommentCardReply();
};

// how a card's review status is drawn
enum class CommentCardStatus {
    None,
    Done,
    Open,
    Declined,
};

struct CommentCard {
    Str author;
    // e.g. "Highlight"
    Str kind;
    Str date;
    Str text;
    // e.g. "Completed (dxf)"; empty for none
    Str status;
    CommentCardStatus statusKind = CommentCardStatus::None;
    Color swatch = kColorUnset;
    Vec<CommentCardReply*> replies;
    bool canReply = false;
    // the owner's
    void* data = nullptr;

    // set by layout, in content coords (before scrolling)
    Rect rect;
    Rect replyBox;

    ~CommentCard();
};

struct CommentCardGroup {
    Str title;
    Vec<CommentCard*> cards;
    bool collapsed = false;

    Rect rect;

    ~CommentCardGroup();
};

struct CommentCardsEvent {
    CommentCards* w = nullptr;
    CommentCard* card = nullptr;
    CommentCardGroup* group = nullptr;
    // screen coords, for a context menu
    Point pt;
    // the reply typed
    Str text;
};

using CommentCardsHandler = Func1<CommentCardsEvent*>;

struct CommentCards : ControlBase {
    struct CreateArgs {
        HWND parent = nullptr;
        PlatformFont* font = nullptr;
        bool isRtl = false;
    };

    Vec<CommentCardGroup*> groups;
    CommentCard* selected = nullptr;
    int scrollY = 0;
    int contentDy = 0;
    // of the font; 0 until measured
    int lineDy = 0;
    Color textCol = kColorUnset;
    Color bgCol = kColorUnset;
    Edit* replyEdit = nullptr;

    // click or keyboard
    CommentCardsHandler onSelected;
    // double click or Enter
    CommentCardsHandler onActivated;
    CommentCardsHandler onContextMenu;
    CommentCardsHandler onDelete;
    CommentCardsHandler onReply;
    CommentCardsHandler onGroupToggled;

    CommentCards();
    ~CommentCards() override;

    HWND Create(const CreateArgs&);
    LRESULT OnMessage(HWND, UINT, WPARAM, LPARAM) override;
    Size GetIdealSize() override;

    void SetGroups(Vec<CommentCardGroup*>& groups);
    void Select(CommentCard*, bool scrollIntoView);
    void SetCardColors(Color text, Color bg);
    void UpdateFont(PlatformFont*);
    void Relayout();
    void FocusReply();

    void Paint(HDC);
    void Scroll(int y);
    void EnsureVisible(Rect r);
    void UpdateScrollbar();
    void PlaceReplyEdit();
    void Notify(const CommentCardsHandler&, CommentCard*, CommentCardGroup*);
    void OnClick(Point, bool dblClick);
    bool OnKey(WPARAM);
    void SelectNext(int dir);
    void ToggleGroup(CommentCardGroup*);
    CommentCard* CardAt(Point);
    CommentCardGroup* GroupHeaderAt(Point);
    CommentCardGroup* GroupOf(CommentCard*);
};

bool CommentCardsTakeKey(HWND, WPARAM);
