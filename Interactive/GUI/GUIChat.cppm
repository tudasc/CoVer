module;

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <expected>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "ContractTree.hpp"

export module GUIChat;
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

class ChatDialog : public QDialog {
    public:

        ChatDialog(LLMChat::ChatSession& chat) : QDialog(GUIManager::Window()), chat{chat} {
            setWindowTitle("LLM Chat");
            QVBoxLayout* layout = new QVBoxLayout(this);

            transcript = new QTextBrowser;
            transcript->setOpenLinks(false);
            QObject::connect(transcript, &QTextBrowser::anchorClicked, this, [this](QUrl const& url) { ShowToolOutput(url.path().toInt()); });
            layout->addWidget(transcript, 1);

            status = new QLabel;
            status->setTextInteractionFlags(Qt::TextSelectableByMouse);
            layout->addWidget(status);

            input = new ChatInput;
            input->setPlaceholderText("Ask about the trace, e.g. why the error is reported or whether it is a false positive (Enter to send, Shift+Enter for a new line)");
            input->setFixedHeight(input->fontMetrics().lineSpacing() * 4 + 12);
            input->on_send = [this] { Send(); };
            layout->addWidget(input);

            QHBoxLayout* buttons = new QHBoxLayout;
            QLabel* hint = new QLabel(qstr(LLMChat::CHAT_ANNOTATIONS_HINT));
            hint->setWordWrap(true);
            buttons->addWidget(hint, 1);
            stop = new QPushButton("Stop");
            QObject::connect(stop, &QPushButton::clicked, this, [this] { this->chat.Cancel(); });
            buttons->addWidget(stop);
            send = new QPushButton("Send");
            send->setDefault(true);
            QObject::connect(send, &QPushButton::clicked, this, [this] { Send(); });
            buttons->addWidget(send);
            QPushButton* close = new QPushButton("Close");
            QObject::connect(close, &QPushButton::clicked, this, &QDialog::reject);
            buttons->addWidget(close);
            layout->addLayout(buttons);

            QTimer* ticker = new QTimer(this); // Updates the elapsed time of a running request
            QObject::connect(ticker, &QTimer::timeout, this, [this] {
                if (this->chat.Busy()) status->setText(qstr(this->chat.Status()));
            });
            ticker->start(1000);

            resize(900, 750);
            chat.SetListener([this] { Update(); });
            Update();
            input->setFocus();
        }

        ~ChatDialog() override {
            chat.SetListener(nullptr);
            chat.Cancel();
        }

    private:
        LLMChat::ChatSession& chat;
        QTextBrowser* transcript;
        QLabel* status;
        ChatInput* input;
        QPushButton* send;
        QPushButton* stop;

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

        // Tool call, clicking it shows the full output
        static void ToolLink(QTextCursor& cursor, int entry_idx, ChatEntry const& entry, QColor const& color) {
            if (!cursor.atStart()) cursor.insertBlock();
            QTextCharFormat link;
            link.setAnchor(true);
            link.setAnchorHref(QString("tool:%1").arg(entry_idx));
            link.setFont(GUIManager::MonoFont());
            link.setForeground(color);
            link.setToolTip("Show the full tool output");
            cursor.insertText(QString::fromUtf8("\u25B8 ") + qstr(entry.text), link);
        }

        void Update() {
            QScrollBar* scroll = transcript->verticalScrollBar();
            bool at_end = scroll->value() == scroll->maximum();
            int position = scroll->value();
            QPalette pal = transcript->palette();
            QColor muted = pal.color(QPalette::PlaceholderText);

            transcript->clear();
            QTextCursor cursor(transcript->document());
            auto block = [&](QString const& text, QColor const& color = QColor(), bool bold = false, bool italic = false, bool mono = false) {
                QTextCharFormat format;
                if (color.isValid()) format.setForeground(color);
                format.setFontWeight(bold ? QFont::Bold : QFont::Normal);
                format.setFontItalic(italic);
                if (mono) format.setFont(GUIManager::MonoFont());
                if (!cursor.atStart()) cursor.insertBlock();
                cursor.insertText(text, format);
            };
            if (chat.Transcript().empty()) {
                block(qstr(LLMChat::CHAT_INTRO), muted, false, true);
            }
            std::vector<ChatEntry> const& entries = chat.Transcript();
            for (int i = 0; i < entries.size(); i++) {
                ChatEntry const& entry = entries[i];
                switch (entry.kind) {
                    case ChatEntry::Kind::USER:
                        block("You", pal.color(QPalette::Link), true);
                        block(qstr(entry.text));
                        break;
                    case ChatEntry::Kind::ASSISTANT:
                        block("Assistant", GUIManager::FulfillmentColor(Fulfillment::FULFILLED), true);
                        cursor.insertBlock();
                        cursor.setCharFormat(QTextCharFormat());
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
                        cursor.insertMarkdown(qstr(entry.text));
#else
                        cursor.insertText(qstr(entry.text));
#endif
                        break;
                    case ChatEntry::Kind::THINKING:
                        block(qstr(entry.text), muted, false, true);
                        break;
                    case ChatEntry::Kind::TOOL:
                        ToolLink(cursor, i, entry, muted);
                        break;
                    case ChatEntry::Kind::INFO:
                        block(qstr(entry.text), GUIManager::FulfillmentColor(Fulfillment::UNKNOWN));
                        break;
                    case ChatEntry::Kind::ERROR:
                        if (!entry.detail.empty()) ToolLink(cursor, i, entry, GUIManager::FulfillmentColor(Fulfillment::BROKEN)); // Failed tool call
                        else block(qstr(entry.text), GUIManager::FulfillmentColor(Fulfillment::BROKEN));
                        break;
                }
            }
            if (chat.Busy()) block("Waiting for the model...", muted, false, true);

            scroll->setValue(at_end ? scroll->maximum() : position);
            status->setText(qstr(chat.Status()));
            send->setEnabled(!chat.Busy());
            stop->setEnabled(chat.Busy());
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
