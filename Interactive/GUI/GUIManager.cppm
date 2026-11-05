module;

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ContractTree.hpp"

export module GUIManager;
import AnalysisReports;
import LLVMModule;
import ErrorMessage;
import InteractiveUtils;
import MCPControl;
import MCPServer;

using namespace ContractTree;

using namespace llvm;

namespace GUIManager {

using Contract = AnalysisReports::Contract;

export bool IsAvailable() {
    return std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY") || std::getenv("QT_QPA_PLATFORM");
}

export QString qstr(std::string const& s) { return QString::fromStdString(s); }

// Runs a task on the GUI thread, usable from any thread
export void Post(std::function<void()> task) {
    QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(task), Qt::QueuedConnection);
}

export QFont MonoFont() { return QFontDatabase::systemFont(QFontDatabase::FixedFont); }

export QColor FulfillmentColor(Fulfillment f) {
    switch (f) {
        case Fulfillment::FULFILLED: return QColor(46, 160, 67);
        case Fulfillment::UNKNOWN: return QColor(210, 153, 34);
        case Fulfillment::BROKEN: return QColor(218, 54, 51);
    }
    return {};
}

// Translucent variant of a color, readable as background in light and dark themes
export QColor Tint(QColor c, int alpha = 90) {
    c.setAlpha(alpha);
    return c;
}

export QIcon StatusIcon(QColor const& c) {
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawEllipse(QRectF(1, 1, 12, 12));
    return QIcon(pixmap);
}

class MainWindow : public QMainWindow {
    public:
        MainWindow() {
            QWidget* central = new QWidget;
            QVBoxLayout* layout = new QVBoxLayout(central);
            header = new QLabel;
            QFont font = header->font();
            font.setPointSizeF(font.pointSizeF() * 1.25);
            font.setBold(true);
            header->setFont(font);
            header->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(header);
            QFrame* line = new QFrame;
            line->setFrameShape(QFrame::HLine);
            line->setFrameShadow(QFrame::Sunken);
            layout->addWidget(line);

            // Shown while no page is active, i.e. while the analysis is running
            stack = new QStackedWidget;
            QLabel* busy = new QLabel("Running analysis...");
            busy->setAlignment(Qt::AlignCenter);
            stack->addWidget(busy);
            layout->addWidget(stack, 1);
            setCentralWidget(central);
            SetTitle("Running analysis");

            QSettings settings("CoVer", "CoVerGUI");
            if (!restoreGeometry(settings.value("geometry").toByteArray())) resize(1400, 900);
        }

        void PushPage(QWidget* page, QString const& title, QEventLoop* loop) {
            pages.push_back({page, title, loop});
            stack->addWidget(page);
            stack->setCurrentWidget(page);
            SetTitle(title);
            if (!quitting && !isVisible()) show();
        }

        void PopPage() {
            stack->removeWidget(pages.back().page);
            pages.pop_back();
            if (pages.empty()) {
                stack->setCurrentIndex(0);
                SetTitle("Running analysis");
            } else {
                stack->setCurrentWidget(pages.back().page);
                SetTitle(pages.back().title);
            }
        }

        void SaveSettings() {
            QSettings settings("CoVer", "CoVerGUI");
            settings.setValue("geometry", saveGeometry());
        }

        bool quitting = false;

    protected:
        // Closing the window ends the interactive session
        void closeEvent(QCloseEvent* e) override {
            SaveSettings();
            quitting = true;
            for (PageEntry const& page : pages) page.loop->quit();
            e->accept();
        }

    private:
        struct PageEntry {
            QWidget* page;
            QString title;
            QEventLoop* loop;
        };
        std::vector<PageEntry> pages;
        QLabel* header;
        QStackedWidget* stack;

        void SetTitle(QString const& title) {
            header->setText("CoVer (Interactive mode) | " + title);
            setWindowTitle("CoVer - " + title);
        }
};

MainWindow* main_window = nullptr;

MainWindow* GetWindow() {
    if (!main_window) {
        static int argc = 1;
        static char arg0[] = "CoVer";
        static char* argv[] = {arg0, nullptr};
        // QApplication adopts the system locale, restore the previous one to keep LLVM output unaffected
        std::string prev_locale = std::setlocale(LC_ALL, nullptr);
        new QApplication(argc, argv);
        std::setlocale(LC_ALL, prev_locale.c_str());
        QApplication::setApplicationName("CoVer");
        QApplication::setQuitOnLastWindowClosed(false);
        main_window = new MainWindow;
    }
    return main_window;
}

export QWidget* Window() { return GetWindow(); }

// Starts the MCP server for the rest of the session unless running, returns an error message on failure
export std::string StartMCP() {
    GetWindow(); // Sets up the application, which runs the posted tool calls
    return MCPControl::Start(Post);
}

// Status and controls of the MCP server, shown below the reports and in the trace debugger
export class MCPPanel : public QWidget {
    public:
        // with_log adds the list of tool calls. start is run by the start button, by default it calls Start.
        MCPPanel(bool with_log, std::function<void()> start = nullptr) {
            status = new QLabel("Start the MCP server to let MCP clients (e.g. coding agents) work through the reports: inspect traces, add annotations and reanalyse.");
            status->setWordWrap(true);
            status->setTextFormat(Qt::RichText);
            status->setTextInteractionFlags(Qt::TextSelectableByMouse);
            start_button = new QPushButton("Start MCP server");
            QObject::connect(start_button, &QPushButton::clicked, this, start ? start : [this] { Start(); });
            copy_button = new QPushButton("Copy header");
            copy_button->setToolTip("Copy the authorization header for the MCP client");
            QObject::connect(copy_button, &QPushButton::clicked, this, [] { QApplication::clipboard()->setText(qstr(MCPControl::AuthHeader())); });
            QHBoxLayout* head = new QHBoxLayout;
            head->addWidget(status, 1);
            head->addWidget(start_button, 0, Qt::AlignTop);
            head->addWidget(copy_button, 0, Qt::AlignTop);
            QVBoxLayout* layout = new QVBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->addLayout(head);
            if (with_log) {
                log = new QPlainTextEdit;
                log->setReadOnly(true);
                log->setFont(MonoFont());
                log->setPlaceholderText("Tool calls of MCP clients are listed here");
                layout->addWidget(log, 1);
                for (MCPServer::Activity const& entry : MCPControl::Log()) AddActivity(entry); // Earlier tool calls of this session
            }
            Update();
        }

        // Starts the server and shows its status, returns an error message on failure
        std::string Start() {
            std::string err = StartMCP();
            if (!err.empty()) status->setText(qstr(err).toHtmlEscaped());
            Update();
            return err;
        }

        // Shows the status once the server runs
        void Update() {
            start_button->setVisible(!MCPControl::Running());
            copy_button->setVisible(MCPControl::Running());
            if (!MCPControl::Running()) return;
            status->setText(QString("<b>URL:</b> %1<br><b>Header:</b> %2<br>Set up your MCP client once with this URL and header (Streamable HTTP transport, see the documentation). "
                                    "The token is kept in your CoVer user settings, remove it there to generate a new one (see the documentation). MCP clients can work through all reports until CoVer is closed. %3 tool calls so far.")
                                .arg(qstr(MCPControl::Url()).toHtmlEscaped(), qstr(MCPControl::AuthHeader()).toHtmlEscaped())
                                .arg(MCPControl::Log().size()));
        }

        void AddActivity(MCPServer::Activity const& entry) {
            if (!log) return;
            QColor color = entry.error ? FulfillmentColor(Fulfillment::UNKNOWN) : QColor();
            QString text = qstr(entry.summary).toHtmlEscaped();
            log->appendHtml(color.isValid() ? QString("<span style=\"color:%1\">%2</span>").arg(color.name(), text) : text);
        }

        // Shows the effect of tool calls, see MCPControl::ScreenScope
        void Show(std::vector<MCPServer::Activity> const& done) {
            for (MCPServer::Activity const& entry : done) AddActivity(entry);
            Update();
        }

    private:
        QLabel* status;
        QPushButton* start_button;
        QPushButton* copy_button;
        QPlainTextEdit* log = nullptr;
};

// Set once the user closed the main window, all pages return as soon as possible
export bool Quitting() { return main_window && main_window->quitting; }

// Shows a page in the main window until Finish() is called or the window is closed. Pages may be nested.
export class PageRun {
    public:
        PageRun(QWidget* page, std::string const& title) : page{page} {
            GetWindow()->PushPage(page, qstr(title), &loop);
        }
        ~PageRun() {
            GetWindow()->PopPage();
            // Delete right away, as the page's handlers may refer to locals of the caller. Silence it while tearing down.
            for (QObject* child : page->findChildren<QObject*>()) child->blockSignals(true);
            delete page;
            QApplication::processEvents();
        }
        void Exec() {
            if (!Quitting()) loop.exec();
        }
        void Finish() { loop.quit(); }

    private:
        QWidget* page;
        QEventLoop loop;
};

// Hide the window while no interaction is possible
void Suspend() {
    GetWindow()->SaveSettings();
    GetWindow()->hide();
    QApplication::processEvents();
}

// Accepts or rejects the dialog depending on the button
QDialogButtonBox* DialogButtons(QDialog* dialog, QDialogButtonBox::StandardButtons buttons) {
    QDialogButtonBox* box = new QDialogButtonBox(buttons);
    QObject::connect(box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return box;
}

// Hides the items of the list not containing the search text
void ConnectFilter(QLineEdit* search, QListWidget* list) {
    QObject::connect(search, &QLineEdit::textChanged, list, [list](QString const& text) {
        for (int i = 0; i < list->count(); i++)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
}

export void ShowLines(std::vector<std::string> const& lines, std::string const& title) {
    QDialog dialog(GetWindow());
    dialog.setWindowTitle(qstr(title));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QPlainTextEdit* text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(MonoFont());
    QStringList qlines;
    for (std::string const& line : lines) qlines << qstr(line);
    text->setPlainText(qlines.join('\n'));
    layout->addWidget(text);
    layout->addWidget(DialogButtons(&dialog, QDialogButtonBox::Close));
    dialog.resize(1000, 600);
    dialog.exec();
}

export std::vector<std::string> SelectMultiple(std::vector<std::string> const& choices, std::string const& title, std::set<std::string> const& preselected) {
    QDialog dialog(GetWindow());
    dialog.setWindowTitle(qstr(title));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLineEdit* search = new QLineEdit;
    search->setPlaceholderText("Type to search...");
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    QListWidget* list = new QListWidget;
    for (std::string const& choice : choices) {
        QListWidgetItem* item = new QListWidgetItem(qstr(choice), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(preselected.contains(choice) ? Qt::Checked : Qt::Unchecked);
    }
    layout->addWidget(list);
    QLabel* count = new QLabel;
    layout->addWidget(count);
    auto updateCount = [&] {
        int checked = 0;
        for (int i = 0; i < list->count(); i++) checked += list->item(i)->checkState() == Qt::Checked;
        count->setText(QString("%1 selected").arg(checked));
    };
    updateCount();
    QObject::connect(list, &QListWidget::itemChanged, &dialog, updateCount);
    QObject::connect(list, &QListWidget::itemActivated, &dialog, [](QListWidgetItem* item) {
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    ConnectFilter(search, list);
    layout->addWidget(DialogButtons(&dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel));
    dialog.resize(500, 600);
    search->setFocus();

    // Cancelling keeps the previous selection
    if (dialog.exec() != QDialog::Accepted) return {preselected.begin(), preselected.end()};
    std::vector<std::string> selected;
    for (int i = 0; i < list->count(); i++) {
        if (list->item(i)->checkState() == Qt::Checked) selected.push_back(list->item(i)->text().toStdString());
    }
    return selected;
}

export struct FormField {
    enum struct Kind { NUMBER, CHOICE, TEXT };
    std::string label;
    Kind kind;
    std::string value; // Initial value
    std::vector<std::pair<std::string, std::string>> choices; // (value, display text), for CHOICE
    bool editable = false; // Whether a CHOICE also accepts custom values
    int min = 0;
    int max = 0;
    int depends_on = -1; // Index of the field which determines the choices of this one
    std::function<std::vector<std::pair<std::string, std::string>>(std::string const&)> update_choices;
};

// Simple input form, returns the field values or nothing if cancelled
export std::optional<std::vector<std::string>> RunForm(std::string const& title, std::string const& description, std::vector<FormField> const& fields) {
    QDialog dialog(GetWindow());
    dialog.setWindowTitle(qstr(title));
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    if (!description.empty()) {
        QLabel* desc = new QLabel(qstr(description));
        desc->setWordWrap(true);
        layout->addWidget(desc);
    }
    QFormLayout* form = new QFormLayout;
    layout->addLayout(form);

    std::vector<QWidget*> widgets;
    for (FormField const& field : fields) {
        QWidget* widget = nullptr;
        switch (field.kind) {
            case FormField::Kind::NUMBER: {
                QSpinBox* spin = new QSpinBox;
                spin->setRange(field.min, field.max);
                spin->setValue(field.value.empty() ? field.min : std::stoi(field.value));
                widget = spin;
                break;
            }
            case FormField::Kind::CHOICE: {
                QComboBox* combo = new QComboBox;
                combo->setEditable(field.editable);
                combo->setMinimumContentsLength(30);
                combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                widget = combo;
                break;
            }
            case FormField::Kind::TEXT:
                widget = new QLineEdit(qstr(field.value));
                break;
        }
        form->addRow(qstr(field.label), widget);
        widgets.push_back(widget);
    }

    auto value = [&](int i) -> std::string {
        if (QSpinBox* spin = qobject_cast<QSpinBox*>(widgets[i])) return std::to_string(spin->value());
        if (QLineEdit* edit = qobject_cast<QLineEdit*>(widgets[i])) return edit->text().trimmed().toStdString();
        QComboBox* combo = qobject_cast<QComboBox*>(widgets[i]);
        if (combo->currentIndex() >= 0 && combo->itemText(combo->currentIndex()) == combo->currentText())
            return combo->currentData().toString().toStdString();
        return combo->currentText().trimmed().toStdString();
    };
    auto fillChoices = [&](int i, std::string const& selected) {
        QComboBox* combo = qobject_cast<QComboBox*>(widgets[i]);
        FormField const& field = fields[i];
        std::vector<std::pair<std::string, std::string>> choices = field.update_choices ? field.update_choices(value(field.depends_on)) : field.choices;
        combo->clear();
        for (std::pair<std::string, std::string> const& choice : choices) combo->addItem(qstr(choice.second), qstr(choice.first));
        int idx = combo->findData(qstr(selected));
        if (idx >= 0) combo->setCurrentIndex(idx);
        else if (field.editable) combo->setEditText(qstr(selected));
    };
    for (int i = 0; i < fields.size(); i++) {
        if (fields[i].kind == FormField::Kind::CHOICE) fillChoices(i, fields[i].value);
    }
    // Refresh dependent choices whenever the field they depend on changes
    for (int i = 0; i < fields.size(); i++) {
        int dep = fields[i].depends_on;
        if (dep < 0) continue;
        auto refill = [&, i] { fillChoices(i, value(i)); };
        if (QSpinBox* spin = qobject_cast<QSpinBox*>(widgets[dep])) QObject::connect(spin, &QSpinBox::valueChanged, &dialog, refill);
        else if (QComboBox* combo = qobject_cast<QComboBox*>(widgets[dep])) QObject::connect(combo, &QComboBox::currentIndexChanged, &dialog, refill);
        else if (QLineEdit* edit = qobject_cast<QLineEdit*>(widgets[dep])) QObject::connect(edit, &QLineEdit::textChanged, &dialog, refill);
    }

    layout->addWidget(DialogButtons(&dialog, QDialogButtonBox::Ok | QDialogButtonBox::Cancel));
    if (!widgets.empty()) widgets.front()->setFocus();

    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    std::vector<std::string> values;
    for (int i = 0; i < fields.size(); i++) values.push_back(value(i));
    return values;
}

// Read-only source view with line numbers
class CodeView : public QPlainTextEdit {
    public:
        CodeView() : gutter{new Gutter(this)} {
            setReadOnly(true);
            setFont(MonoFont());
            setLineWrapMode(QPlainTextEdit::NoWrap);
            QObject::connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(GutterWidth(), 0, 0, 0); });
            QObject::connect(this, &QPlainTextEdit::updateRequest, this, [this](QRect const& rect, int dy) {
                if (dy) gutter->scroll(0, dy);
                else gutter->update(0, rect.y(), gutter->width(), rect.height());
            });
            setViewportMargins(GutterWidth(), 0, 0, 0);
        }

        void ShowFile(std::string const& file, int line) {
            QFile f(qstr(file));
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                setPlainText(qstr("Could not open file " + file));
                setExtraSelections({});
                return;
            }
            setPlainText(QString::fromUtf8(f.readAll()));
            QTextBlock block = document()->findBlockByNumber(line - 1);
            if (!block.isValid()) return;
            QTextCursor cursor(block);
            QTextEdit::ExtraSelection selection;
            selection.format.setBackground(Tint(FulfillmentColor(Fulfillment::BROKEN)));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.cursor = cursor;
            setExtraSelections({selection});
            setTextCursor(cursor);
            centerCursor();
        }

    protected:
        void resizeEvent(QResizeEvent* e) override {
            QPlainTextEdit::resizeEvent(e);
            QRect cr = contentsRect();
            gutter->setGeometry(QRect(cr.left(), cr.top(), GutterWidth(), cr.height()));
        }

    private:
        class Gutter : public QWidget {
            public:
                Gutter(CodeView* view) : QWidget(view), view{view} {}
                QSize sizeHint() const override { return QSize(view->GutterWidth(), 0); }
            protected:
                void paintEvent(QPaintEvent* e) override { view->PaintGutter(e); }
            private:
                CodeView* view;
        };
        Gutter* gutter;

        int GutterWidth() const {
            int digits = QString::number(std::max(1, blockCount())).size();
            return 16 + fontMetrics().horizontalAdvance('9') * digits;
        }

        void PaintGutter(QPaintEvent* e) {
            QPainter p(gutter);
            p.fillRect(e->rect(), palette().color(QPalette::AlternateBase));
            p.setPen(palette().color(QPalette::PlaceholderText));
            QTextBlock block = firstVisibleBlock();
            int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
            int bottom = top + qRound(blockBoundingRect(block).height());
            while (block.isValid() && top <= e->rect().bottom()) {
                if (block.isVisible() && bottom >= e->rect().top())
                    p.drawText(0, top, gutter->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(block.blockNumber() + 1));
                block = block.next();
                top = bottom;
                bottom = top + qRound(blockBoundingRect(block).height());
            }
        }
};

// Contract string and formula tree, optionally allowing to launch the worklist debugger for leaf formulas
class ContractView : public QWidget {
    public:
        ContractView(AnalysisReports::FormulaDebugHandlers& debugHandlers, std::function<void(std::shared_ptr<ContractExpression>)> on_debug) : debugHandlers{debugHandlers}, on_debug{on_debug} {
            QVBoxLayout* layout = new QVBoxLayout(this);
            layout->setContentsMargins(0, 0, 0, 0);
            function = new QLabel;
            QFont font = function->font();
            font.setBold(true);
            function->setFont(font);
            function->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(function);
            contract = new QLabel;
            contract->setFont(MonoFont());
            contract->setWordWrap(true);
            contract->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(contract);
            tree = new QTreeWidget;
            tree->setHeaderLabels({"Formula", "Status"});
            tree->header()->setStretchLastSection(false);
            tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
            tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            layout->addWidget(tree, 1);

            if (on_debug) {
                QHBoxLayout* buttons = new QHBoxLayout;
                hint = new QLabel;
                hint->setWordWrap(true);
                buttons->addWidget(hint, 1);
                debug = new QPushButton("Debug Worklist Analysis...");
                buttons->addWidget(debug);
                layout->addLayout(buttons);
                QObject::connect(debug, &QPushButton::clicked, this, [this] { LaunchDebugger(); });
                QObject::connect(tree, &QTreeWidget::itemActivated, this, [this] { LaunchDebugger(); });
                QObject::connect(tree, &QTreeWidget::currentItemChanged, this, [this] { UpdateDebugButton(); });
            }
            Clear("Select a contract to inspect it");
        }

        void Clear(QString const& message) {
            function->setText(message);
            contract->clear();
            tree->clear();
            formulas.clear();
            UpdateDebugButton();
        }

        // Show contract, and select the given formula (or its first debuggable violated leaf)
        void SetContract(Contract const& C, ContractFormula const* focus = nullptr) {
            Clear("");
            function->setText(qstr("Function: " + C.F->getName().str()));
            contract->setText(qstr(C.ContractString));
            QTreeWidgetItem* focus_item = nullptr;
            for (auto const& [name, scope] : {std::pair{"Preconditions", C.Data.Pre}, std::pair{"Postconditions", C.Data.Post}}) {
                if (!scope || scope->Children.empty()) continue;
                QTreeWidgetItem* root = new QTreeWidgetItem(tree, {name});
                QFont font = root->font(0);
                font.setBold(true);
                root->setFont(0, font);
                for (std::shared_ptr<ContractFormula> const& form : scope->Children) AddFormula(root, form, focus, focus_item);
            }
            tree->expandAll();
            if (focus_item) {
                // Prefer the first violated subformula that can be debugged
                if (QTreeWidgetItem* leaf = FindDebuggable(focus_item)) focus_item = leaf;
                tree->setCurrentItem(focus_item);
                tree->scrollToItem(focus_item);
            }
            UpdateDebugButton();
        }

    private:
        AnalysisReports::FormulaDebugHandlers& debugHandlers;
        std::function<void(std::shared_ptr<ContractExpression>)> on_debug;
        std::map<QTreeWidgetItem*, std::shared_ptr<ContractFormula>> formulas;
        QLabel* function;
        QLabel* contract;
        QTreeWidget* tree;
        QLabel* hint = nullptr;
        QPushButton* debug = nullptr;

        void AddFormula(QTreeWidgetItem* parent, std::shared_ptr<ContractFormula> const& form, ContractFormula const* focus, QTreeWidgetItem*& focus_item) {
            QTreeWidgetItem* item = new QTreeWidgetItem(parent, {qstr(form->ExprStr), qstr(FulfillmentStr(*form->Status))});
            item->setBackground(1, Tint(FulfillmentColor(*form->Status)));
            if (form->Message) item->setToolTip(0, qstr(*form->Message));
            formulas[item] = form;
            if (form.get() == focus) focus_item = item;
            for (std::shared_ptr<ContractFormula> const& child : form->Children) AddFormula(item, child, focus, focus_item);
        }

        QTreeWidgetItem* FindDebuggable(QTreeWidgetItem* item) {
            std::shared_ptr<ContractFormula> const& form = formulas.at(item);
            if (*form->Status != Fulfillment::FULFILLED && debugHandlers.contains(form.get())) return item;
            for (int i = 0; i < item->childCount(); i++) {
                if (QTreeWidgetItem* found = FindDebuggable(item->child(i))) return found;
            }
            return nullptr;
        }

        std::shared_ptr<ContractExpression> Selected() {
            if (!formulas.contains(tree->currentItem())) return nullptr;
            std::shared_ptr<ContractFormula> form = formulas[tree->currentItem()];
            if (!debugHandlers.contains(form.get())) return nullptr;
            return std::static_pointer_cast<ContractExpression>(form);
        }

        void UpdateDebugButton() {
            if (!debug) return;
            debug->setEnabled(Selected() != nullptr);
            hint->setText(Selected() ? "Inspect the worklist analysis trace of the selected formula"
                                     : "Select a leaf formula with worklist analysis info to debug it");
        }

        void LaunchDebugger() {
            if (std::shared_ptr<ContractExpression> expr = Selected()) on_debug(expr);
        }
};

QWidget* VBox(std::vector<QWidget*> widgets, QString const& title = "") {
    QWidget* box = title.isEmpty() ? new QWidget : new QGroupBox(title);
    QVBoxLayout* layout = new QVBoxLayout(box);
    if (title.isEmpty()) layout->setContentsMargins(0, 0, 0, 0);
    for (QWidget* w : widgets) layout->addWidget(w);
    return box;
}

export void StartMenu(std::vector<AnalysisReports::Contract> const& Contracts, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, double parse_time) {
    GetWindow(); // Sets up the application
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    QSplitter* splitter = new QSplitter(Qt::Horizontal);

    QLineEdit* search = new QLineEdit;
    search->setPlaceholderText("Filter contracts...");
    search->setClearButtonEnabled(true);
    QListWidget* contracts = new QListWidget;
    for (AnalysisReports::Contract const& contract : Contracts) {
        QListWidgetItem* item = new QListWidgetItem(qstr(contract.F->getName().str()), contracts);
        item->setToolTip(qstr(contract.ContractString));
    }
    splitter->addWidget(VBox({search, contracts}, "Contracts"));
    ContractView* details = new ContractView(debugHandlers, nullptr);
    splitter->addWidget(VBox({details}, "Contract Details"));
    splitter->setStretchFactor(1, 2);
    layout->addWidget(splitter, 1);

    QHBoxLayout* buttons = new QHBoxLayout;
    buttons->addStretch();
    QPushButton* start = new QPushButton("Start Analysis");
    start->setDefault(true);
    buttons->addWidget(start);
    layout->addLayout(buttons);

    ConnectFilter(search, contracts);
    QObject::connect(contracts, &QListWidget::currentRowChanged, page, [&](int row) {
        if (row >= 0) details->SetContract(Contracts[row]);
    });

    PageRun run(page, InteractiveUtils::startMenuTitle(Contracts.size(), parse_time));
    QObject::connect(start, &QPushButton::clicked, page, [&] { run.Finish(); });
    start->setFocus();
    run.Exec();
    if (Quitting()) {
        errs() << "CoVer: Interactive session aborted\n";
        InteractiveUtils::removeInteractiveFiles();
        std::exit(EXIT_FAILURE);
    }
}

export bool ResultsScreen(std::vector<InteractiveUtils::ReportedFormula> const& formulas, AnalysisReports::FormulaReports const& reports, AnalysisReports::FormulaDebugHandlers& debugHandlers, double runtime) {
    GetWindow(); // Sets up the application
    QWidget* page = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(page);
    QSplitter* splitter = new QSplitter(Qt::Horizontal);

    QListWidget* violations = new QListWidget;
    violations->setWordWrap(true);
    for (InteractiveUtils::ReportedFormula const& formula : formulas) {
        QListWidgetItem* item = new QListWidgetItem(qstr(formula.Summary()), violations);
        item->setIcon(StatusIcon(FulfillmentColor(*formula.formula->Status)));
        item->setToolTip(qstr(FulfillmentStr(*formula.formula->Status) + ": " + formula.formula->ExprStr));
    }
    if (formulas.empty()) {
        QListWidgetItem* item = new QListWidgetItem("No reported errors!", violations);
        item->setFlags(Qt::NoItemFlags);
    }
    splitter->addWidget(VBox({violations}, "Reported Errors"));

    // Details of the selected report
    QPlainTextEdit* report = new QPlainTextEdit;
    report->setReadOnly(true);
    QListWidget* refs = new QListWidget;
    QSplitter* report_split = new QSplitter(Qt::Horizontal);
    report_split->addWidget(VBox({report}, "Full Report"));
    report_split->addWidget(VBox({refs}, "References"));
    report_split->setStretchFactor(0, 2);

    QLabel* source_file = new QLabel;
    source_file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    CodeView* source = new CodeView;

    bool reanalyse = false;
    std::optional<PageRun> run;

    // MCP clients work through the reports: they open them, also from within the debugger of another report
    bool navigating = false; // A debugger is open. Its teardown runs events, which must not open the next one before it is gone.
    std::function<void()> open_requested = [&] {
        if (navigating) return; // Picked up by the loop below once the debugger closed
        navigating = true;
        while (run && !reanalyse && MCPControl::PendingAction().kind == MCPControl::Action::Kind::OPEN_REPORT) {
            violations->setCurrentRow(MCPControl::PendingAction().report);
            if (MCPControl::OpenPendingReport()) {
                reanalyse = true;
                run->Finish();
            }
        }
        navigating = false;
    };
    ContractView* contract = new ContractView(debugHandlers, [&](std::shared_ptr<ContractExpression> expr) {
        int row = violations->currentRow();
        if (row < 0 || row >= formulas.size()) return;
        navigating = true;
        bool debug_reanalyse = debugHandlers[expr.get()](AnalysisReports::DebugContext(*formulas[row].contract, *formulas[row].formula, *expr, reports));
        navigating = false;
        if (debug_reanalyse) {
            reanalyse = true;
            run->Finish();
            return;
        }
        open_requested(); // An MCP client may have requested another report from within the debugger
    });

    QSplitter* details = new QSplitter(Qt::Vertical);
    details->addWidget(report_split);
    details->addWidget(VBox({source_file, source}, "Source Preview"));
    details->addWidget(VBox({contract}, "Contract"));
    details->setStretchFactor(1, 2);
    details->setStretchFactor(2, 1);
    splitter->addWidget(details);
    splitter->setStretchFactor(1, 3);
    layout->addWidget(splitter, 1);

    QHBoxLayout* buttons = new QHBoxLayout;
    MCPPanel* mcp = new MCPPanel(false);
    buttons->addWidget(mcp, 1);
    QPushButton* exit = new QPushButton("Exit Tool");
    buttons->addWidget(exit, 0, Qt::AlignTop);
    layout->addLayout(buttons);
    std::vector<FileReference> cur_refs;
    QObject::connect(refs, &QListWidget::currentRowChanged, page, [&](int row) {
        if (row < 0 || row >= cur_refs.size()) return;
        source_file->setText(qstr(std::format("{}:{}:{}", cur_refs[row].file, cur_refs[row].line, cur_refs[row].column)));
        source->ShowFile(cur_refs[row].file, cur_refs[row].line);
    });
    QObject::connect(violations, &QListWidget::currentRowChanged, page, [&](int row) {
        if (row < 0 || row >= formulas.size()) return;
        std::shared_ptr<ContractFormula> const& form = formulas[row].formula;
        QStringList texts;
        cur_refs.clear();
        refs->clear();
        if (auto it = reports.find(form.get()); it != reports.end()) {
            for (ErrorMessage const& err : it->second) {
                texts << qstr(err.text);
                for (FileReference const& ref : err.references) {
                    refs->addItem(qstr(std::format("{}:{}:{}", ref.file, ref.line, ref.column)));
                    cur_refs.push_back(ref);
                }
            }
        }
        report->setPlainText(texts.join("\n\n"));
        source_file->clear();
        source->clear();
        if (!cur_refs.empty()) refs->setCurrentRow(0);
        contract->SetContract(*formulas[row].contract, form.get());
    });

    run.emplace(page, InteractiveUtils::resultsTitle(!formulas.empty(), runtime));
    QObject::connect(exit, &QPushButton::clicked, page, [&] { run->Finish(); });
    if (!formulas.empty()) violations->setCurrentRow(0);
    QTimer::singleShot(0, page, [&] {
        open_requested(); // E.g. the report debugged before the reanalysis, see MCPControl::ResultsScope
        MCPControl::RunWaiting(); // Tool calls which arrived during the analysis
    });
    {
        // Only while the page runs: its teardown deletes the widgets and processes events
        MCPControl::ScreenScope mcp_screen([&](std::vector<MCPServer::Activity> const& done) {
            mcp->Show(done);
            open_requested();
        });
        run->Exec();
    }
    run.reset();
    Suspend();
    return reanalyse && !Quitting();
}

}
