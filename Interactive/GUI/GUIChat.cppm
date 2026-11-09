module;

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextTable>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "ContractTree.hpp"

export module GUIChat;
import std;
import InteractiveSession;
import GUIManager;
import LLMChat;
import LLMClient;

// Qt frontend of the LLM chat
namespace GUIChat {

using GUIManager::qstr;
using LLMChat::ChatEntry;
using ContractTree::Fulfillment;

// Message input, Enter sends and Shift+Enter starts a new line
class ChatInput : public QPlainTextEdit {
    public:
        std::function<void()> on_send;

    protected:
        void keyPressEvent(QKeyEvent* e) override {
            if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && !(e->modifiers() & Qt::ShiftModifier)) {
                on_send();
                return;
            }
            QPlainTextEdit::keyPressEvent(e);
        }
};

QColor ToQColor(LLMChat::Rgb c) { return QColor(c.r, c.g, c.b); }

// Icon of the desktop's icon theme, the style's own icon if the theme has none
QIcon ThemeIcon(QWidget* widget, char const* name, QStyle::StandardPixmap fallback) {
    return QIcon::fromTheme(name, widget->style()->standardIcon(fallback));
}

// Thin line between the panels of the chat
QFrame* Separator() {
    QFrame* line = new QFrame;
    line->setFixedHeight(1);
    line->setBackgroundRole(QPalette::Mid);
    line->setAutoFillBackground(true);
    return line;
}

// Thin bar with a moving segment while a request runs, painted itself since styles draw progress bars this thin poorly
class BusyBar : public QWidget {
    public:
        BusyBar() {
            setFixedHeight(3);
            QObject::connect(&timer, &QTimer::timeout, this, [this] {
                offset = (offset + 8) % (width() + SEGMENT);
                update();
            });
        }

        void SetRunning(bool running) {
            if (running == timer.isActive()) return;
            if (running) timer.start(30);
            else timer.stop();
            offset = 0;
            update();
        }

    protected:
        void paintEvent(QPaintEvent*) override {
            if (!timer.isActive()) return;
            QPainter p(this);
            p.fillRect(rect(), GUIManager::Tint(ToQColor(LLMChat::StateColor(LLMChat::ChatState::BUSY)), 60));
            p.fillRect(QRect(offset - SEGMENT, 0, SEGMENT, height()), ToQColor(LLMChat::StateColor(LLMChat::ChatState::BUSY)));
        }

    private:
        static constexpr int SEGMENT = 120;
        QTimer timer;
        int offset = 0;
};

class ChatDialog : public QDialog {
    public:

        ChatDialog(LLMChat::ChatSession& chat) : QDialog(GUIManager::Window()), chat{chat} {
            setWindowTitle("LLM Chat");
            QVBoxLayout* layout = new QVBoxLayout(this); // Panels from edge to edge: header, transcript, input
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(0);

            QHBoxLayout* header = new QHBoxLayout; // Connection state, model and token usage
            header->setContentsMargins(PANEL_MARGIN, 8, PANEL_MARGIN, 5);
            header->setSpacing(SPACING); // Nested layouts would take the spacing of the outer one
            state_dot = new QLabel;
            header->addWidget(state_dot);
            model = new QLabel;
            QFont bold = model->font();
            bold.setBold(true);
            model->setFont(bold);
            header->addWidget(model);
            header->addStretch(1);
            usage = new QLabel;
            usage->setForegroundRole(QPalette::PlaceholderText);
            usage->setTextInteractionFlags(Qt::TextSelectableByMouse);
            header->addWidget(usage);
            layout->addLayout(header);

            busy_bar = new BusyBar;
            layout->addWidget(busy_bar);
            layout->addWidget(Separator());

            transcript = new QTextBrowser;
            transcript->setFrameShape(QFrame::NoFrame);
            transcript->document()->setDocumentMargin(PANEL_MARGIN);
            transcript->setOpenLinks(false);
            QObject::connect(transcript, &QTextBrowser::anchorClicked, this, [this](QUrl const& url) { ShowToolOutput(url.path().toInt()); });
            layout->addWidget(transcript, 1);
            layout->addWidget(Separator());

            QVBoxLayout* footer = new QVBoxLayout;
            footer->setContentsMargins(PANEL_MARGIN, 10, PANEL_MARGIN, 10);
            footer->setSpacing(SPACING);
            QHBoxLayout* compose = new QHBoxLayout;
            compose->setSpacing(SPACING);
            input = new ChatInput;
            input->setPlaceholderText("Ask about the trace, e.g. why the error is reported or whether it is a false positive (Enter to send, Shift+Enter for a new line)");
            input->setFixedHeight(input->fontMetrics().lineSpacing() * 4 + 12);
            input->on_send = [this] { Send(); };
            compose->addWidget(input, 1);
            send_stop = new QToolButton; // Sends, or stops the running request
            send_stop->setIconSize(QSize(22, 22));
            send_stop->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
            send_stop->setMinimumWidth(44);
            QObject::connect(send_stop, &QToolButton::clicked, this, [this] {
                if (this->chat.Busy()) this->chat.Cancel();
                else Send();
            });
            compose->addWidget(send_stop);
            footer->addLayout(compose);

            QHBoxLayout* buttons = new QHBoxLayout;
            buttons->setSpacing(SPACING);
            QLabel* hint = new QLabel(qstr(LLMChat::CHAT_ANNOTATIONS_HINT));
            hint->setWordWrap(true);
            hint->setForegroundRole(QPalette::PlaceholderText);
            buttons->addWidget(hint, 1);
            export_log = new QPushButton("Export...");
            export_log->setToolTip("Save the chat log as JSON");
            QObject::connect(export_log, &QPushButton::clicked, this, [this] { GUIManager::ExportChatLog(this); });
            buttons->addWidget(export_log);
            close = new QPushButton("Close");
            QObject::connect(close, &QPushButton::clicked, this, &QDialog::reject);
            buttons->addWidget(close);
            footer->addLayout(buttons);
            layout->addLayout(footer);

            QTimer* ticker = new QTimer(this); // Updates the elapsed time of a running request
            QObject::connect(ticker, &QTimer::timeout, this, [this] {
                if (this->chat.Busy()) UpdateControls();
            });
            ticker->start(1000);

            resize(900, 750);
            chat.SetListener([this] { Update(); });
            ApplyTheme();
            input->setFocus();
        }

        ~ChatDialog() override {
            chat.SetListener(nullptr);
            chat.Cancel();
        }

    protected:
        void changeEvent(QEvent* e) override {
            QDialog::changeEvent(e);
            // The transcript has the colors of the theme built in, and the icons come from the theme or style
            if (transcript && (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)) ApplyTheme();
        }

    private:
        static constexpr int PANEL_MARGIN = 12;
        static constexpr int SPACING = 6;
        static constexpr int CARD_BAR = 3; // Width of the accent bar of a message
        static constexpr int CARD_PADDING = 8;

        LLMChat::ChatSession& chat;
        QLabel* state_dot = nullptr;
        QLabel* model = nullptr;
        QLabel* usage = nullptr;
        BusyBar* busy_bar = nullptr;
        QTextBrowser* transcript = nullptr;
        ChatInput* input = nullptr;
        QToolButton* send_stop = nullptr;
        QPushButton* export_log = nullptr;
        QPushButton* close = nullptr;

        void Send() {
            std::string text = input->toPlainText().trimmed().toStdString();
            if (chat.Busy() || text.empty()) return;
            input->clear();
            chat.Send(text);
            transcript->verticalScrollBar()->setValue(transcript->verticalScrollBar()->maximum());
        }

        void ShowToolOutput(int entry) {
            std::vector<ChatEntry> const& entries = chat.Transcript();
            if (entry < 0 || entry >= entries.size()) return;
            std::vector<std::string> lines;
            std::istringstream stream(entries[entry].detail);
            for (std::string line; std::getline(stream, line);) lines.push_back(line);
            GUIManager::ShowLines(lines, entries[entry].text);
        }

        void ApplyTheme() {
            export_log->setIcon(ThemeIcon(this, "document-save-as", QStyle::SP_DialogSaveButton));
            close->setIcon(ThemeIcon(this, "window-close", QStyle::SP_DialogCloseButton));
            Update();
        }

        // Header and buttons, which change with the state of the chat
        void UpdateControls() {
            state_dot->setPixmap(GUIManager::StatusIcon(ToQColor(LLMChat::StateColor(chat.State()))).pixmap(14, 14));
            std::string name = chat.Model();
            model->setText(name.empty() ? "Not connected" : qstr(name));
            std::string text = chat.Usage();
            if (chat.Busy()) text = std::format("Waiting {}s | {}", chat.Waiting().count(), text);
            usage->setText(qstr(text));
            usage->setVisible(!name.empty());
            busy_bar->SetRunning(chat.Busy());
            if (chat.Busy()) {
                send_stop->setIcon(ThemeIcon(this, "process-stop", QStyle::SP_BrowserStop));
                send_stop->setToolTip("Stop the running request");
            } else {
                send_stop->setIcon(ThemeIcon(this, "mail-send", QStyle::SP_ArrowRight));
                send_stop->setToolTip("Send (Enter)");
            }
        }

        void Update() {
            QScrollBar* scroll = transcript->verticalScrollBar();
            bool at_end = scroll->value() == scroll->maximum();
            int position = scroll->value();
            QPalette pal = transcript->palette();
            QColor muted = pal.color(QPalette::PlaceholderText);
            int tint = pal.color(QPalette::Base).lightness() < 128 ? 45 : 25; // Translucent colors fade more on dark backgrounds
            auto color = [&](ChatEntry const& entry) {
                std::optional<LLMChat::Rgb> rgb = LLMChat::EntryColor(entry);
                return rgb ? ToQColor(*rgb) : muted;
            };

            transcript->clear();
            QTextCursor cursor(transcript->document());
            // Starts a paragraph, reusing an empty one such as the one after a card
            auto paragraph = [&](QTextBlockFormat const& format) {
                if (cursor.block().text().isEmpty()) cursor.setBlockFormat(format);
                else cursor.insertBlock(format);
            };
            // A line outside of the messages, aligned with their content
            auto line = [&](QString const& text, QTextCharFormat const& format) {
                QTextBlockFormat indent;
                indent.setLeftMargin(CARD_BAR + CARD_PADDING);
                paragraph(indent);
                cursor.insertText(text, format);
            };
            auto note = [&](QString const& text) { // Dimmed status line
                QTextCharFormat format;
                format.setForeground(muted);
                format.setFontItalic(true);
                line(text, format);
            };
            // Tool call, clicking it shows the full output
            auto tool = [&](int entry_idx, ChatEntry const& entry) {
                QTextCharFormat link;
                link.setAnchor(true);
                link.setAnchorHref(QString("tool:%1").arg(entry_idx));
                link.setFont(GUIManager::MonoFont());
                link.setForeground(color(entry));
                link.setToolTip("Show the full tool output");
                line(QString::fromUtf8("▸ ") + qstr(entry.text), link);
            };
            // A message: a bar in its accent color on the left, the content next to it, on a tinted background if requested.
            // Leaves the cursor in the content, with an optional bold label as first line.
            auto card = [&](QColor const& accent, bool tinted, QString const& label) {
                paragraph(QTextBlockFormat());
                QTextTableFormat format;
                format.setBorderStyle(QTextFrameFormat::BorderStyle_None);
                format.setBorder(0);
                format.setCellSpacing(0);
                format.setCellPadding(0);
                format.setTopMargin(4);
                format.setBottomMargin(4);
                format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
                format.setColumnWidthConstraints({QTextLength(QTextLength::FixedLength, CARD_BAR), QTextLength(QTextLength::VariableLength, 0)});
                QTextTable* table = cursor.insertTable(1, 2, format);
                QTextTableCellFormat bar;
                bar.setBackground(accent);
                table->cellAt(0, 0).setFormat(bar);
                QTextTableCellFormat content;
                content.setPadding(CARD_PADDING);
                if (tinted) content.setBackground(GUIManager::Tint(accent, tint));
                table->cellAt(0, 1).setFormat(content);
                cursor = table->cellAt(0, 1).firstCursorPosition();
                if (label.isEmpty()) return;
                QTextCharFormat heading;
                heading.setForeground(accent);
                heading.setFontWeight(QFont::Bold);
                cursor.insertText(label, heading);
                cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
            };
            auto end_card = [&] { cursor.movePosition(QTextCursor::End); };

            if (chat.Transcript().empty()) note(qstr(LLMChat::CHAT_INTRO));
            std::vector<ChatEntry> const& entries = chat.Transcript();
            for (int i = 0; i < entries.size(); i++) {
                ChatEntry const& entry = entries[i];
                switch (entry.kind) {
                    case ChatEntry::Kind::USER:
                        card(color(entry), true, "You");
                        cursor.insertText(qstr(entry.text), QTextCharFormat());
                        end_card();
                        break;
                    case ChatEntry::Kind::ASSISTANT:
                        card(color(entry), false, "Assistant");
                        cursor.setCharFormat(QTextCharFormat());
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
                        cursor.insertMarkdown(qstr(entry.text));
#else
                        cursor.insertText(qstr(entry.text));
#endif
                        end_card();
                        break;
                    case ChatEntry::Kind::THINKING:
                        note(qstr(LLMChat::ThinkingLabel(entry)));
                        break;
                    case ChatEntry::Kind::TOOL:
                        tool(i, entry);
                        break;
                    case ChatEntry::Kind::INFO:
                        card(color(entry), true, "");
                        cursor.insertText(qstr(entry.text), QTextCharFormat());
                        end_card();
                        break;
                    case ChatEntry::Kind::ERROR:
                        if (!entry.detail.empty()) { // Failed tool call
                            tool(i, entry);
                            break;
                        }
                        card(color(entry), true, "");
                        cursor.insertText(qstr(entry.text), QTextCharFormat());
                        end_card();
                        break;
                }
            }
            if (chat.Busy()) note("Waiting for the model...");

            scroll->setValue(at_end ? scroll->maximum() : position);
            UpdateControls();
        }
};

// Shows the chat until closed, which cancels a running request
void ShowChat(LLMChat::ChatSession& chat) {
    ChatDialog dialog(chat);
    dialog.exec();
}

// Form for the connection settings, the error tells why it is shown. False if cancelled.
bool EditSettings(LLMClient::Config& config, std::string const& error) {
    QDialog dialog(GUIManager::Window());
    dialog.setWindowTitle("LLM Chat Settings");
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* intro = new QLabel(qstr(LLMChat::SETTINGS_INTRO));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QLabel* message = new QLabel(qstr(error));
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette pal = message->palette();
    pal.setColor(QPalette::WindowText, GUIManager::FulfillmentColor(Fulfillment::BROKEN));
    message->setPalette(pal);
    message->setVisible(!error.empty());
    layout->addWidget(message);

    LLMChat::SettingsForm form = LLMChat::MakeSettingsForm(config);
    QFormLayout* rows = new QFormLayout; // A row per field, in the order of the form
    for (LLMChat::SettingsField& field : form) {
        QLineEdit* edit = new QLineEdit(qstr(field.value));
        edit->setPlaceholderText(qstr(field.placeholder));
        if (field.password) edit->setEchoMode(QLineEdit::Password);
        QObject::connect(edit, &QLineEdit::textChanged, &dialog, [&field](QString const& text) { field.value = text.toStdString(); });
        rows->addRow(qstr(field.label), edit);
    }
    layout->addLayout(rows);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    layout->addWidget(buttons); // Before setting the default button, which only takes effect within the dialog
    buttons->addButton("Save and Connect", QDialogButtonBox::AcceptRole)->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        std::expected<LLMClient::Config, std::string> made = LLMChat::ConfigFromForm(form);
        if (!made) {
            message->setText(qstr(made.error()));
            message->setVisible(true);
            return;
        }
        config = *made;
        dialog.accept();
    });
    dialog.resize(750, dialog.sizeHint().height());
    int focus = !error.empty() && config.base_url.empty() ? LLMChat::FIELD_URL : LLMChat::FIELD_MODEL; // The server URL if it is missing, otherwise the model
    rows->itemAt(focus, QFormLayout::FieldRole)->widget()->setFocus();
    return dialog.exec() == QDialog::Accepted;
}

// Shows the running connection test until it is done, false if cancelled
bool WaitForTest(LLMChat::ConnectionTest const& test, LLMClient::Config const& config) {
    QDialog dialog(GUIManager::Window());
    dialog.setWindowTitle("LLM Chat");
    QVBoxLayout* layout = new QVBoxLayout(&dialog);
    QLabel* label = new QLabel(qstr("Testing the connection to " + config.model + " at " + config.base_url + "...\n"
                                    "This can take a while if the server has to load the model first."));
    label->setWordWrap(true);
    layout->addWidget(label);
    QProgressBar* progress = new QProgressBar;
    progress->setRange(0, 0); // Busy indicator
    layout->addWidget(progress);
    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    QTimer* poll = new QTimer(&dialog);
    QObject::connect(poll, &QTimer::timeout, &dialog, [&] {
        if (test.Result()) dialog.accept();
    });
    poll->start(100);
    dialog.resize(500, dialog.sizeHint().height());
    return dialog.exec() == QDialog::Accepted;
}

// Tests the connection and shows the chat, asking for the settings first if needed
void OpenChat(LLMChat::ChatSession& chat) {
    LLMChat::Open(chat, {.settings = EditSettings, .testing = WaitForTest, .chat = [&] { ShowChat(chat); }});
}

// The chat command of the trace debugger, see LLMChat::ChatCommand
export std::function<void()> ChatCommand(InteractiveSession::TraceSession& session) {
    return LLMChat::ChatCommand(session, GUIManager::Post, OpenChat);
}

// Lets the user edit and test the settings, returns the outcome
export std::string OpenSettings() {
    return LLMChat::Configure({.settings = EditSettings, .testing = WaitForTest});
}

} // namespace GUIChat
