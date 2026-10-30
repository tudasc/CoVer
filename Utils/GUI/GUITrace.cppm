module;

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneContextMenuEvent>
#include <QGraphicsSceneHoverEvent>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ContractTree.hpp"

export module GUITrace;
import LLVMModule;
import ContractPassUtility;
import InteractiveUtils;
import InteractiveCmds;
import InteractiveSession;
import GUIManager;

using namespace llvm;

namespace GUITrace {

using InteractiveCmds::CmdFrontend;
using InteractiveCmds::CmdInfo;
using InteractiveSession::CmdResult;
using InteractiveSession::CmdResultCode;
using InteractiveSession::History;
using InteractiveSession::TraceBlockView;
using InteractiveSession::TraceChildView;
using InteractiveSession::TraceInstrView;
using InteractiveSession::TraceSession;
using GUIManager::qstr;
using ContractTree::Fulfillment;

constexpr qreal NODE_MIN_WIDTH = 280;
constexpr qreal NODE_MAX_WIDTH = 760;
constexpr qreal GHOST_WIDTH = 230;
constexpr qreal PAD = 10;
constexpr qreal HEADER_HEIGHT = 26;
constexpr qreal ROW_GAP = 70;
constexpr qreal COL_GAP = 50;

QColor InfoColor(std::string const& info) {
    if (info.find("ERROR") != std::string::npos) return GUIManager::FulfillmentColor(Fulfillment::BROKEN);
    static std::vector<QColor> const palette = {
        QColor(56, 139, 253), QColor(46, 160, 67), QColor(163, 113, 247),
        QColor(210, 153, 34), QColor(219, 97, 162), QColor(57, 170, 180),
    };
    return palette[std::hash<std::string>{}(info) % palette.size()];
}

qreal DrawBadge(QPainter* p, QPointF const& pos, QString const& text, QColor const& color) {
    QFont font = p->font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    font.setBold(true);
    QFontMetricsF fm(font);
    QRectF rect(pos.x(), pos.y() - fm.height() / 2 - 2, fm.horizontalAdvance(text) + 12, fm.height() + 4);
    p->save();
    p->setPen(Qt::NoPen);
    p->setBrush(color);
    p->drawRoundedRect(rect, rect.height() / 2, rect.height() / 2);
    p->setPen(Qt::white);
    p->setFont(font);
    p->drawText(rect, Qt::AlignCenter, text);
    p->restore();
    return rect.width();
}

class TraceGraph;
class EdgeItem;

class NodeItem : public QGraphicsItem {
    public:
        NodeItem(TraceGraph* graph, std::pair<void const*, int> key) : key{key}, graph{graph} {
            setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
            setAcceptHoverEvents(true);
            setCursor(Qt::OpenHandCursor);
        }
        QRectF boundingRect() const override { return Rect().adjusted(-3, -3, 3, 3); }
        QRectF Rect() const { return QRectF(QPointF(0, 0), size); }
        qreal Width() const { return size.width(); }
        qreal Height() const { return size.height(); }
        QPointF Anchor(Qt::Edge edge) const {
            switch (edge) {
                case Qt::TopEdge: return pos() + QPointF(size.width() / 2, 0);
                case Qt::BottomEdge: return pos() + QPointF(size.width() / 2, size.height());
                case Qt::LeftEdge: return pos() + QPointF(0, size.height() / 2);
                case Qt::RightEdge: return pos() + QPointF(size.width(), size.height() / 2);
            }
            return pos();
        }
        void AddEdge(EdgeItem* edge) { edges.push_back(edge); }

        std::pair<void const*, int> key; // Identity to remember positions of nodes moved by the user

    protected:
        QVariant itemChange(GraphicsItemChange change, QVariant const& value) override;
        void mousePressEvent(QGraphicsSceneMouseEvent* e) override {
            press_pos = pos();
            setCursor(Qt::ClosedHandCursor);
            QGraphicsItem::mousePressEvent(e);
        }
        void mouseReleaseEvent(QGraphicsSceneMouseEvent* e) override;

        TraceGraph* graph;
        QSizeF size;
        std::vector<EdgeItem*> edges;
        QPointF press_pos;
};

class EdgeItem : public QGraphicsItem {
    public:
        enum struct Style { CHAIN, SIBLING, LOOP };

        EdgeItem(NodeItem* from, NodeItem* to, Style style, QString label) : from{from}, to{to}, style{style}, label{label} {
            setZValue(-1);
            from->AddEdge(this);
            to->AddEdge(this);
            Adjust();
        }

        void Adjust() {
            prepareGeometryChange();
            QPointF start, end, c1, c2;
            if (style == Style::LOOP) {
                // Loops go around the left side, to stay clear of sibling nodes
                start = from->Anchor(Qt::LeftEdge);
                end = to->Anchor(Qt::LeftEdge);
                qreal x = std::min(start.x(), end.x()) - 80 - 0.05 * std::abs(end.y() - start.y());
                c1 = QPointF(x, start.y());
                c2 = QPointF(x, end.y());
            } else {
                start = from->Anchor(Qt::BottomEdge);
                end = to->Anchor(Qt::TopEdge);
                qreal dy = std::max<qreal>(40, std::abs(end.y() - start.y()) / 2);
                c1 = start + QPointF(0, dy);
                c2 = end - QPointF(0, dy);
            }
            path = QPainterPath(start);
            path.cubicTo(c1, c2, end);

            QPointF dir = end - c2;
            qreal len = std::hypot(dir.x(), dir.y());
            dir = len > 0 ? dir / len : QPointF(0, 1);
            QPointF normal(-dir.y(), dir.x());
            arrow = QPolygonF({end, end - dir * 11 + normal * 5, end - dir * 11 - normal * 5});

            QFont font = QApplication::font();
            font.setPointSizeF(font.pointSizeF() * 0.85);
            QFontMetricsF fm(font);
            QPointF mid = path.pointAtPercent(0.5);
            label_rect = label.isEmpty() ? QRectF() : QRectF(0, 0, fm.horizontalAdvance(label) + 10, fm.height() + 4);
            if (style == Style::LOOP) label_rect.moveCenter(mid);
            else label_rect.moveTopLeft(mid + QPointF(8, -label_rect.height() / 2));
            update();
        }

        QRectF boundingRect() const override {
            return path.boundingRect().united(arrow.boundingRect()).united(label_rect).adjusted(-4, -4, 4, 4);
        }

        void paint(QPainter* p, QStyleOptionGraphicsItem const*, QWidget*) override {
            QPalette pal = QApplication::palette();
            QColor color = pal.color(QPalette::Text);
            color.setAlpha(style == Style::CHAIN ? 200 : 130);
            Qt::PenStyle pen_style = style == Style::CHAIN ? Qt::SolidLine : style == Style::SIBLING ? Qt::DashLine : Qt::DotLine;
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(QPen(color, style == Style::CHAIN ? 2 : 1.6, pen_style));
            p->setBrush(Qt::NoBrush);
            p->drawPath(path);
            p->setPen(Qt::NoPen);
            p->setBrush(color);
            p->drawPolygon(arrow);
            if (label.isEmpty()) return;
            QFont font = p->font();
            font.setPointSizeF(font.pointSizeF() * 0.85);
            p->setFont(font);
            QColor bg = pal.color(QPalette::Window);
            bg.setAlpha(220);
            p->setBrush(bg);
            p->drawRoundedRect(label_rect, 4, 4);
            p->setPen(pal.color(QPalette::Text));
            p->drawText(label_rect, Qt::AlignCenter, label);
        }

    private:
        NodeItem* from;
        NodeItem* to;
        Style style;
        QString label;
        QPainterPath path;
        QPolygonF arrow;
        QRectF label_rect;
};

QVariant NodeItem::itemChange(GraphicsItemChange change, QVariant const& value) {
    if (change == ItemPositionHasChanged) {
        for (EdgeItem* edge : edges) edge->Adjust();
    }
    return QGraphicsItem::itemChange(change, value);
}

// Owns the scene and lays out the trace blocks. Commands are passed back to the debugger.
class TraceGraph {
    public:
        struct Callbacks {
            std::function<void(std::string const&)> command;
            std::function<void(int)> select;
            std::function<void(QMenu*, int, int)> block_menu; // Fill menu for block and instruction (-1 if none)
        };

        TraceGraph(Callbacks callbacks) : callbacks{callbacks}, scene{new QGraphicsScene} {
            QObject::connect(scene, &QGraphicsScene::selectionChanged, scene, [this] { SelectionChanged(); });
        }
        ~TraceGraph() { delete scene; }

        QGraphicsScene* Scene() { return scene; }

        void Rebuild(TraceSession& session);

        void Select(int block) {
            if (block < 0 || block >= block_nodes.size()) return;
            updating = true;
            scene->clearSelection();
            block_nodes[block]->setSelected(true);
            updating = false;
        }

        QGraphicsItem* Node(int block) { return block < block_nodes.size() ? block_nodes[block] : nullptr; }

        void ResetLayout() { manual_pos.clear(); }

        void NodesMoved() {
            for (QGraphicsItem* item : scene->selectedItems()) {
                if (NodeItem* node = dynamic_cast<NodeItem*>(item)) manual_pos[node->key] = node->pos();
            }
            scene->setSceneRect(scene->sceneRect().united(scene->itemsBoundingRect().adjusted(-200, -200, 200, 200)));
        }

        // Run commands only after the current event is handled, as they rebuild the scene
        void Command(std::string const& cmd) {
            QTimer::singleShot(0, scene, [this, cmd] { callbacks.command(cmd); });
        }

        void BlockMenu(int block, int instr, QPoint const& screen_pos) {
            QMenu menu;
            callbacks.block_menu(&menu, block, instr);
            menu.exec(screen_pos);
        }

    private:
        Callbacks callbacks;
        QGraphicsScene* scene;
        std::vector<NodeItem*> block_nodes;
        std::map<std::pair<void const*, int>, QPointF> manual_pos;
        bool updating = false;

        void SelectionChanged();
};

void NodeItem::mouseReleaseEvent(QGraphicsSceneMouseEvent* e) {
    QGraphicsItem::mouseReleaseEvent(e);
    setCursor(Qt::OpenHandCursor);
    if (pos() != press_pos) graph->NodesMoved();
}

class BlockNode : public NodeItem {
    public:
        BlockNode(TraceGraph* graph, TraceBlockView const& block, std::vector<TraceInstrView> instrs, bool as_source)
            : NodeItem(graph, {block.entry, block.jump_to ? 1 : 0}), block{block}, instrs{std::move(instrs)} {
            QFontMetricsF fm(QApplication::font());
            QFontMetricsF mono_fm(GUIManager::MonoFont());
            line_height = fm.height();
            instr_height = mono_fm.height();
            lines = {qstr("from " + block.start_loc), qstr("to " + block.end_loc)};
            if (block.jump_to) lines << QString("then jump to block %1").arg(*block.jump_to);

            size_t info_width = 0;
            for (TraceInstrView const& instr : this->instrs) info_width = std::max(info_width, instr.info.size());
            for (TraceInstrView const& instr : this->instrs)
                instr_lines << qstr(std::format("{:0>3} {:<{}} {}", instr.index, instr.info, info_width, as_source ? instr.source : instr.ir));

            qreal width = fm.horizontalAdvance(QString("Block %1").arg(block.index)) + fm.horizontalAdvance(qstr(block.info)) + fm.horizontalAdvance(qstr(block.function)) + 70;
            for (QString const& line : lines) width = std::max(width, fm.horizontalAdvance(line));
            for (QString const& line : instr_lines) width = std::max(width, mono_fm.horizontalAdvance(line));
            size.setWidth(std::clamp(width + 2 * PAD, NODE_MIN_WIDTH, NODE_MAX_WIDTH));
            size.setHeight(HEADER_HEIGHT + PAD + lines.size() * line_height + (instr_lines.empty() ? 0 : PAD + instr_lines.size() * instr_height) + PAD);
            setToolTip(qstr(block.description));
        }

        int Index() const { return block.index; }

        void paint(QPainter* p, QStyleOptionGraphicsItem const*, QWidget*) override {
            QPalette pal = QApplication::palette();
            QColor accent = InfoColor(block.info);
            QRectF rect = Rect();
            p->setRenderHint(QPainter::Antialiasing);

            // Body and header band
            p->setPen(Qt::NoPen);
            p->setBrush(pal.color(QPalette::Base));
            p->drawRoundedRect(rect, 8, 8);
            p->save();
            p->setClipRect(QRectF(0, 0, rect.width(), HEADER_HEIGHT));
            p->setBrush(GUIManager::Tint(accent, 70));
            p->drawRoundedRect(rect, 8, 8);
            p->restore();
            QPen border(isSelected() ? pal.color(QPalette::Highlight) : GUIManager::Tint(accent, 220), isSelected() ? 3 : 1.5);
            if (block.jump_to) border.setStyle(Qt::DashLine);
            p->setPen(border);
            p->setBrush(Qt::NoBrush);
            p->drawRoundedRect(rect, 8, 8);

            // Header: block number, analysis info, function
            QFont bold = p->font();
            bold.setBold(true);
            p->setFont(bold);
            p->setPen(pal.color(QPalette::Text));
            QString title = QString("Block %1").arg(block.index);
            QRectF title_rect(PAD, 0, rect.width() - 2 * PAD, HEADER_HEIGHT);
            p->drawText(title_rect, Qt::AlignVCenter | Qt::AlignLeft, title);
            qreal x = PAD + QFontMetricsF(bold).horizontalAdvance(title) + 8;
            x += DrawBadge(p, QPointF(x, HEADER_HEIGHT / 2), qstr(block.info), accent) + 8;
            p->setFont(QApplication::font());
            QFontMetricsF fm(p->font());
            p->drawText(QRectF(x, 0, rect.width() - x - PAD, HEADER_HEIGHT), Qt::AlignVCenter | Qt::AlignRight,
                        fm.elidedText(qstr(block.function), Qt::ElideMiddle, rect.width() - x - PAD));

            // Locations
            qreal y = HEADER_HEIGHT + PAD / 2;
            for (QString const& line : lines) {
                p->drawText(QRectF(PAD, y, rect.width() - 2 * PAD, line_height), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(line, Qt::ElideMiddle, rect.width() - 2 * PAD));
                y += line_height;
            }

            // Instructions of expanded blocks
            if (instr_lines.empty()) return;
            y += PAD / 2;
            p->setPen(GUIManager::Tint(pal.color(QPalette::Text), 60));
            p->drawLine(QPointF(PAD, y), QPointF(rect.width() - PAD, y));
            y += PAD / 2;
            p->setFont(GUIManager::MonoFont());
            QFontMetricsF mono_fm(p->font());
            for (int i = 0; i < instr_lines.size(); i++) {
                QRectF line_rect(PAD / 2, y, rect.width() - PAD, instr_height);
                if (i == hovered) p->fillRect(line_rect, GUIManager::Tint(pal.color(QPalette::Highlight), 60));
                else if (i % 2) p->fillRect(line_rect, GUIManager::Tint(pal.color(QPalette::AlternateBase), 160));
                p->setPen(i == hovered ? pal.color(QPalette::Text) : GUIManager::Tint(InfoColor(instrs[i].info), 255));
                p->drawText(line_rect.adjusted(PAD / 2, 0, -PAD / 2, 0), Qt::AlignVCenter | Qt::AlignLeft, mono_fm.elidedText(instr_lines[i], Qt::ElideRight, rect.width() - 2 * PAD));
                y += instr_height;
            }
        }

    protected:
        void mouseDoubleClickEvent(QGraphicsSceneMouseEvent*) override {
            graph->Command(std::format("{} {}", block.expanded ? "collapse" : "expand", block.index));
        }
        void contextMenuEvent(QGraphicsSceneContextMenuEvent* e) override {
            setSelected(true);
            graph->BlockMenu(block.index, InstrAt(e->pos()), e->screenPos());
        }
        void hoverMoveEvent(QGraphicsSceneHoverEvent* e) override {
            int instr = InstrAt(e->pos());
            if (instr == hovered) return;
            hovered = instr;
            setToolTip(instr >= 0 ? qstr(std::format("{}.{:0>3} [{}] {}\n{}", block.index, instrs[instr].index, instrs[instr].info, instrs[instr].ir, instrs[instr].location))
                                  : qstr(block.description));
            update();
        }
        void hoverLeaveEvent(QGraphicsSceneHoverEvent*) override {
            hovered = -1;
            update();
        }

    private:
        TraceBlockView block;
        std::vector<TraceInstrView> instrs;
        QStringList lines;
        QStringList instr_lines;
        qreal line_height;
        qreal instr_height;
        int hovered = -1;

        int InstrAt(QPointF const& pos) const {
            if (instr_lines.empty()) return -1;
            qreal top = HEADER_HEIGHT + PAD + lines.size() * line_height;
            int idx = std::floor((pos.y() - top) / instr_height);
            return idx >= 0 && idx < instr_lines.size() ? idx : -1;
        }
};

// Alternative child (predecessor) of a block that is not part of the currently shown trace
class SiblingNode : public NodeItem {
    public:
        SiblingNode(TraceGraph* graph, int block, TraceChildView const& child) : NodeItem(graph, {child.entry, 2}), block{block}, child{child} {
            QFontMetricsF fm(QApplication::font());
            size = QSizeF(GHOST_WIDTH, HEADER_HEIGHT + 2 * fm.height() + PAD);
            setToolTip(qstr(std::format("Child {} of block {}: {} in {}\nDouble-click to show this child instead", child.child, block, child.location, child.function)));
        }

        void paint(QPainter* p, QStyleOptionGraphicsItem const*, QWidget*) override {
            QPalette pal = QApplication::palette();
            QRectF rect = Rect();
            p->setRenderHint(QPainter::Antialiasing);
            p->setBrush(GUIManager::Tint(pal.color(QPalette::Base), 200));
            p->setPen(QPen(isSelected() ? pal.color(QPalette::Highlight) : GUIManager::Tint(pal.color(QPalette::Text), 120), isSelected() ? 2.5 : 1.2, Qt::DashLine));
            p->drawRoundedRect(rect, 8, 8);
            p->setPen(GUIManager::Tint(pal.color(QPalette::Text), 200));
            QString title = QString("Child %1").arg(child.child);
            p->drawText(QRectF(PAD, 0, rect.width(), HEADER_HEIGHT), Qt::AlignVCenter | Qt::AlignLeft, title);
            qreal x = PAD + QFontMetricsF(p->font()).horizontalAdvance(title) + 8;
            DrawBadge(p, QPointF(x, HEADER_HEIGHT / 2), qstr(child.info), GUIManager::Tint(InfoColor(child.info), 170));
            QFontMetricsF fm(p->font());
            qreal y = HEADER_HEIGHT;
            for (std::string const& line : {child.location, child.function}) {
                p->drawText(QRectF(PAD, y, rect.width() - 2 * PAD, fm.height()), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(qstr(line), Qt::ElideMiddle, rect.width() - 2 * PAD));
                y += fm.height();
            }
        }

    protected:
        void mouseDoubleClickEvent(QGraphicsSceneMouseEvent*) override { Follow(); }
        void contextMenuEvent(QGraphicsSceneContextMenuEvent* e) override {
            QMenu menu;
            QObject::connect(menu.addAction(QString("Show child %1 of block %2").arg(child.child).arg(block)), &QAction::triggered, [this] { Follow(); });
            menu.exec(e->screenPos());
        }

    private:
        int block;
        TraceChildView child;

        void Follow() { graph->Command(std::format("child {} {}", block, child.child)); }
};

void TraceGraph::SelectionChanged() {
    if (updating) return;
    for (QGraphicsItem* item : scene->selectedItems()) {
        if (BlockNode* node = dynamic_cast<BlockNode*>(item)) {
            callbacks.select(node->Index());
            return;
        }
    }
}

void TraceGraph::Rebuild(TraceSession& session) {
    std::vector<TraceBlockView> const& blocks = session.Blocks();
    updating = true;
    scene->clear();
    block_nodes.clear();

    // Trace blocks form a chain, each block followed by its selected child
    qreal y = 0;
    for (TraceBlockView const& block : blocks) {
        BlockNode* node = new BlockNode(this, block, block.expanded ? session.Instructions(block.index) : std::vector<TraceInstrView>{}, session.ExpandAsSource());
        scene->addItem(node);
        node->setPos(manual_pos.contains(node->key) ? manual_pos[node->key] : QPointF(-node->Width() / 2, y));
        y += node->Height() + ROW_GAP;
        block_nodes.push_back(node);
    }
    for (int i = 0; i + 1 < blocks.size(); i++) {
        TraceBlockView const& block = blocks[i];
        QString label = qstr(block.kind);
        if (block.children.size() > 1) label += QString(" | child %1/%2").arg(block.selected_child).arg(block.children.size() - 1);
        scene->addItem(new EdgeItem(block_nodes[i], block_nodes[i + 1], EdgeItem::Style::CHAIN, label));

        // Siblings of the selected child are placed beside it
        qreal x = block_nodes[i + 1]->pos().x() + block_nodes[i + 1]->Width() + COL_GAP;
        for (TraceChildView const& child : block.children) {
            if (child.child == block.selected_child) continue;
            SiblingNode* sibling = new SiblingNode(this, i, child);
            scene->addItem(sibling);
            sibling->setPos(manual_pos.contains(sibling->key) ? manual_pos[sibling->key] : QPointF(x, block_nodes[i + 1]->pos().y()));
            x += sibling->Width() + COL_GAP / 2;
            scene->addItem(new EdgeItem(block_nodes[i], sibling, EdgeItem::Style::SIBLING, QString("child %1").arg(child.child)));
        }
    }
    for (TraceBlockView const& block : blocks) {
        if (block.jump_to)
            scene->addItem(new EdgeItem(block_nodes[block.index], block_nodes[*block.jump_to], EdgeItem::Style::LOOP, QString("repeats block %1").arg(*block.jump_to)));
    }
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-200, -200, 200, 200));
    updating = false;
}

class GraphView : public QGraphicsView {
    public:
        GraphView(QGraphicsScene* scene) : QGraphicsView(scene) {
            setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
            setDragMode(QGraphicsView::ScrollHandDrag);
            setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            setViewportUpdateMode(QGraphicsView::BoundingRectViewportUpdate);
        }

        void Zoom(qreal factor) {
            qreal current = transform().m11();
            factor = std::clamp(current * factor, 0.1, 4.0) / current;
            scale(factor, factor);
        }

        void ResetZoom() { setTransform(QTransform()); }

        void Fit() {
            fitInView(scene()->itemsBoundingRect().adjusted(-20, -20, 20, 20), Qt::KeepAspectRatio);
            if (transform().m11() > 1) ResetZoom();
        }

    protected:
        void wheelEvent(QWheelEvent* e) override {
            if (e->modifiers() & Qt::ControlModifier) {
                Zoom(std::pow(1.0015, e->angleDelta().y()));
                e->accept();
                return;
            }
            QGraphicsView::wheelEvent(e);
        }
};

// Command input with history navigation
class CommandLine : public QLineEdit {
    public:
        CommandLine(History const& history) : history{history} {}

    protected:
        void keyPressEvent(QKeyEvent* e) override {
            if (e->key() != Qt::Key_Up && e->key() != Qt::Key_Down) return QLineEdit::keyPressEvent(e);
            if (e->key() == Qt::Key_Up) pos = pos < 0 ? (int)history.size() - 1 : std::max(0, pos - 1);
            else pos = pos < 0 || pos + 1 >= history.size() ? -1 : pos + 1;
            setText(pos < 0 ? "" : qstr(history[pos].first));
        }
        void focusInEvent(QFocusEvent* e) override {
            pos = -1;
            QLineEdit::focusInEvent(e);
        }

    private:
        History const& history;
        int pos = -1;
};

QString ResultText(CmdResultCode code) {
    switch (code) {
        case CmdResultCode::INVALID_INPUT: return "Invalid Input";
        case CmdResultCode::INVALID_SYNTAX: return "Invalid Syntax";
        case CmdResultCode::UNKNOWN_COMMAND: return "Unknown Command";
        default: return "Success";
    }
}

QColor ResultColor(CmdResultCode code) {
    switch (code) {
        case CmdResultCode::INVALID_INPUT:
        case CmdResultCode::INVALID_SYNTAX: return GUIManager::FulfillmentColor(Fulfillment::UNKNOWN);
        case CmdResultCode::UNKNOWN_COMMAND: return GUIManager::FulfillmentColor(Fulfillment::BROKEN);
        default: return QColor();
    }
}

class TraceDebugger {
    public:
        TraceDebugger(TraceSession& session) : session{session}, blocks{session.Blocks()}, history{session.GetHistory()}, commands{InteractiveCmds::GetTraceCommands()} {
            frontend = {
                .showLines = GUIManager::ShowLines,
                .selectMultiple = GUIManager::SelectMultiple,
                .showHistory = [this](History const&) { bottom_tabs->setCurrentWidget(history_table); },
            };
            graph = new TraceGraph({
                .command = [this](std::string const& cmd) { Execute(cmd); },
                .select = [this](int block) { SelectBlock(block, false); },
                .block_menu = [this](QMenu* menu, int block, int instr) { FillBlockMenu(menu, block, instr); },
            });
            BuildPage();
        }

        ~TraceDebugger() { delete graph; }

        bool Exec() {
            GUIManager::PageRun page_run(page, "JumpTrace");
            run = &page_run;
            Refresh();
            UpdateHistory();
            SelectBlock(0, true);
            console->setFocus();
            page_run.Exec();
            run = nullptr;
            return reanalyse && !GUIManager::Quitting();
        }

    private:
        TraceSession& session;
        std::vector<TraceBlockView> const& blocks;
        History const& history;
        std::vector<CmdInfo> const& commands;
        CmdFrontend frontend;

        int selected = 0;
        bool reanalyse = false;
        GUIManager::PageRun* run = nullptr;

        TraceGraph* graph;
        QWidget* page;
        GraphView* view;
        QTreeWidget* block_list;
        QLabel* block_title;
        QLabel* block_desc;
        QComboBox* child_select;
        QPushButton* expand_button;
        QTableWidget* instr_table;
        QPlainTextEdit* source_text;
        QTabWidget* bottom_tabs;
        QPlainTextEdit* output;
        QTableWidget* history_table;
        CommandLine* console;
        QLabel* last_result;

        void Execute(std::string const& cmd) {
            QTimer::singleShot(0, page, [this, cmd] { RunCommand(cmd); });
        }

        void RunCommand(std::string const& cmd) {
            if (!run) return;
            CmdResult res = InteractiveCmds::ExecuteCommand(session, cmd, frontend);
            QString line = "<b>&gt;&gt;&gt; " + qstr(cmd).toHtmlEscaped() + "</b>";
            if (!res.res.empty()) {
                QColor color = ResultColor(res.code);
                line += color.isValid() ? QString("<br><span style=\"color:%1\">%2</span>").arg(color.name(), qstr(res.res).toHtmlEscaped()) : "<br>" + qstr(res.res).toHtmlEscaped();
            }
            output->appendHtml(line);
            last_result->setText(qstr(res.res));
            QColor color = ResultColor(res.code);
            last_result->setStyleSheet(color.isValid() ? QString("color: %1;").arg(color.name()) : "");
            UpdateHistory();
            if (res.code == CmdResultCode::SUCCESS_EXIT) return run->Finish();
            if (res.code == CmdResultCode::SUCCESS_REANALYSE) {
                reanalyse = true;
                return run->Finish();
            }
            Refresh();
        }

        void Refresh() {
            selected = std::clamp(selected, 0, (int)blocks.size() - 1);
            QSignalBlocker blocker(block_list);
            block_list->clear();
            for (TraceBlockView const& block : blocks) {
                QTreeWidgetItem* item = new QTreeWidgetItem(block_list, {QString::number(block.index), qstr(block.info), qstr(block.description)});
                item->setIcon(1, GUIManager::StatusIcon(InfoColor(block.info)));
                item->setToolTip(2, qstr(block.description));
            }
            graph->Rebuild(session);
            SelectBlock(selected, false);
            view->ensureVisible(graph->Node(selected), 40, 40);
        }

        void SelectBlock(int block, bool center) {
            if (block < 0 || block >= blocks.size()) return;
            selected = block;
            {
                QSignalBlocker blocker(block_list);
                block_list->setCurrentItem(block_list->topLevelItem(block));
            }
            graph->Select(block);
            if (center) view->centerOn(graph->Node(block));
            UpdateDetails();
        }

        void UpdateDetails() {
            TraceBlockView const& block = blocks[selected];
            block_title->setText(qstr(std::format("Block {} in {} [{}]", block.index, block.function, block.info)));
            block_desc->setText(qstr(block.description));
            expand_button->setText(block.expanded ? "Collapse in graph" : "Expand in graph");
            {
                QSignalBlocker blocker(child_select);
                child_select->clear();
                for (TraceChildView const& child : block.children)
                    child_select->addItem(GUIManager::StatusIcon(InfoColor(child.info)), qstr(std::format("Child {}: {} [{}]", child.child, child.location, child.info)));
                child_select->setCurrentIndex(block.selected_child);
                child_select->setEnabled(block.children.size() > 1);
            }
            std::vector<TraceInstrView> const& instrs = session.Instructions(selected);
            instr_table->setRowCount(instrs.size());
            for (int i = 0; i < instrs.size(); i++) {
                std::vector<QString> cells = {QString::number(instrs[i].index), qstr(instrs[i].info), qstr(instrs[i].ir), qstr(instrs[i].location)};
                for (int col = 0; col < cells.size(); col++) {
                    QTableWidgetItem* item = new QTableWidgetItem(cells[col]);
                    item->setToolTip(cells[col]);
                    if (col == 2) item->setFont(GUIManager::MonoFont());
                    instr_table->setItem(i, col, item);
                }
                instr_table->item(i, 1)->setIcon(GUIManager::StatusIcon(InfoColor(instrs[i].info)));
            }
            QStringList lines;
            for (InteractiveUtils::BlockLine const& line : session.BlockLines(selected, true)) lines << qstr(line.text);
            source_text->setPlainText(lines.join('\n'));
        }

        void UpdateHistory() {
            history_table->setRowCount(history.size());
            for (int i = 0; i < history.size(); i++) {
                QTableWidgetItem* code = new QTableWidgetItem(ResultText(history[i].second.code));
                QColor color = ResultColor(history[i].second.code);
                if (color.isValid()) code->setBackground(GUIManager::Tint(color));
                code->setToolTip(qstr(history[i].second.res));
                history_table->setItem(i, 0, new QTableWidgetItem(QString::number(i)));
                history_table->setItem(i, 1, code);
                history_table->setItem(i, 2, new QTableWidgetItem(qstr(history[i].first)));
            }
            history_table->scrollToBottom();
        }

        int SelectedInstr() {
            int row = instr_table->currentRow();
            return row >= 0 && row < session.Instructions(selected).size() ? session.Instructions(selected)[row].index : 0;
        }

        // Form helpers for the annotation commands
        GUIManager::FormField BlockField(int block) {
            return {.label = "Block", .kind = GUIManager::FormField::Kind::NUMBER, .value = std::to_string(block), .min = 0, .max = (int)blocks.size() - 1};
        }

        GUIManager::FormField InstrField(int block_field, int instr) {
            return {.label = "Instruction", .kind = GUIManager::FormField::Kind::CHOICE, .value = std::to_string(instr), .depends_on = block_field,
                    .update_choices = [this](std::string const& block) {
                        std::vector<std::pair<std::string, std::string>> choices;
                        for (TraceInstrView const& in : session.Instructions(std::stoi(block)))
                            choices.push_back({std::to_string(in.index), std::format("{:0>3}: {}{}", in.index, in.indirect_call ? "[indirect call] " : "", in.ir)});
                        return choices;
                    }};
        }

        GUIManager::FormField ValueField(std::string const& label, int block_field, std::string const& value) {
            return {.label = label, .kind = GUIManager::FormField::Kind::CHOICE, .value = value, .editable = true, .depends_on = block_field,
                    .update_choices = [this](std::string const& block) {
                        std::vector<std::pair<std::string, std::string>> choices;
                        std::set<std::string> seen;
                        for (TraceInstrView const& in : session.Instructions(std::stoi(block))) {
                            if (!in.value.empty() && seen.insert(in.value).second) choices.push_back({in.value, in.value});
                        }
                        // Values defined outside of the block, but referenced in it
                        for (TraceInstrView const& in : session.Instructions(std::stoi(block))) {
                            for (std::string const& op : in.operands)
                                if (seen.insert(op).second) choices.push_back({op, op});
                        }
                        return choices;
                    }};
        }

        GUIManager::FormField GroupField() {
            GUIManager::FormField field = {.label = "Alias group", .kind = GUIManager::FormField::Kind::CHOICE};
            for (auto const& [idx, group] : ContractPassUtility::getAliasAnnots())
                field.choices.push_back({std::to_string(idx), std::format("Group {} ({}, {} values)", idx, group.areAliasing ? "should alias" : "should not alias", group.members.size())});
            return field;
        }

        GUIManager::FormField AliasTypeField(std::string const& value) {
            return {.label = "Values", .kind = GUIManager::FormField::Kind::CHOICE, .value = value, .choices = {{"yes", "should alias"}, {"no", "should not alias"}}};
        }

        bool HasAliasGroups() {
            if (!ContractPassUtility::getAliasAnnots().empty()) return true;
            QMessageBox::information(page, "No alias groups", "There are no alias groups yet. Create one first.");
            return false;
        }

        void Form(std::string const& title, std::string const& description, std::vector<GUIManager::FormField> const& fields, std::function<std::string(std::vector<std::string> const&)> to_cmd) {
            std::optional<std::vector<std::string>> values = GUIManager::RunForm(title, description, fields);
            if (values) Execute(to_cmd(*values));
        }

        void FpTargetDialog(int block, int instr) {
            Form("Function Pointer Targets", "Select an indirect call, then choose its possible targets.", {BlockField(block), InstrField(0, instr)},
                 [](auto const& v) { return std::format("fp-target {} {}", v[0], v[1]); });
        }

        void AllocDialog(int block, int instr) {
            Form("Allocation Annotation", "Annotate the pointer produced by an instruction as allocated with at least the given size in bytes (0: unknown size).",
                 {BlockField(block), InstrField(0, instr), {.label = "Size", .kind = GUIManager::FormField::Kind::TEXT, .value = "0"}},
                 [](auto const& v) { return std::format("alloc-create {} {} {}", v[0], v[1], v[2]); });
        }

        void AliasCreateDialog(int block, std::string const& value) {
            Form("Create Alias Group", "Annotate whether two values should alias. This creates a new alias group.",
                 {AliasTypeField("yes"), BlockField(block), ValueField("First value", 1, value), BlockField(block), ValueField("Second value", 3, "")},
                 [](auto const& v) { return std::format("alias-create {} {} {} {} {}", v[0], v[1], v[2], v[3], v[4]); });
        }

        void AliasAddDialog(int block, std::string const& value) {
            if (!HasAliasGroups()) return;
            Form("Add to Alias Group", "Add a value to an existing alias group.", {GroupField(), BlockField(block), ValueField("Value", 1, value)},
                 [](auto const& v) { return std::format("alias-add {} {} {}", v[0], v[1], v[2]); });
        }

        void AliasTypeDialog() {
            if (!HasAliasGroups()) return;
            Form("Alias Group Type", "Set whether the values of an alias group should alias.", {GroupField(), AliasTypeField("yes")},
                 [](auto const& v) { return std::format("alias-type {} {}", v[0], v[1]); });
        }

        void AliasRemoveDialog() {
            if (!HasAliasGroups()) return;
            GUIManager::FormField member = {.label = "Member", .kind = GUIManager::FormField::Kind::CHOICE, .depends_on = 0,
                .update_choices = [](std::string const& group) {
                    std::vector<std::pair<std::string, std::string>> choices;
                    int idx = std::stoi(group);
                    if (!ContractPassUtility::getAliasAnnots().contains(idx)) return choices;
                    int num = 0;
                    for (Value const* V : ContractPassUtility::getAliasAnnots().at(idx).members) {
                        std::string out;
                        raw_string_ostream strstream(out);
                        V->print(strstream);
                        choices.push_back({std::to_string(num), std::format("{:0>3}: {}", num, out)});
                        num++;
                    }
                    num = 0;
                    for (CallBase const* CB : ContractPassUtility::getAliasEpochs(idx)) {
                        choices.push_back({std::format("e{}", num), std::format("e{:0>2}: epoch in {} at {}", num, CB->getFunction()->getName().str(), ContractPassUtility::getInstrLocStr(CB))});
                        num++;
                    }
                    return choices;
                }};
            Form("Remove from Alias Group", "Remove a value or an epoch from an alias group.", {GroupField(), member},
                 [](auto const& v) { return std::format("alias-rm {} {}", v[0], v[1]); });
        }

        void AliasEpochDialog(int block, int instr) {
            if (!HasAliasGroups()) return;
            Form("New Alias Epoch", "Start a new epoch for an alias group before the given instruction.", {GroupField(), BlockField(block), InstrField(1, instr)},
                 [](auto const& v) { return std::format("alias-epoch {} {} {}", v[0], v[1], v[2]); });
        }

        void FileCommand(std::string const& cmd, bool save, QString const& title, QString const& default_name) {
            QString path = save ? QFileDialog::getSaveFileName(page, title, default_name) : QFileDialog::getOpenFileName(page, title);
            if (path.isEmpty()) return;
            if (path.contains(QRegularExpression("\\s"))) {
                QMessageBox::warning(page, title, "File paths must not contain whitespace.");
                return;
            }
            Execute(cmd + " " + path.toStdString());
        }

        void FillInstrMenu(QMenu* menu, int block, int instr) {
            TraceInstrView const& in = session.Instructions(block)[instr];
            menu->addSection(qstr(std::format("Instruction {}.{:0>3}", block, in.index)));
            if (in.indirect_call)
                QObject::connect(menu->addAction("Annotate function pointer targets..."), &QAction::triggered, [=, this] { Execute(std::format("fp-target {} {}", block, in.index)); });
            QObject::connect(menu->addAction("Annotate as allocated..."), &QAction::triggered, [=, this] { AllocDialog(block, in.index); });
            QObject::connect(menu->addAction("Start alias epoch before this instruction..."), &QAction::triggered, [=, this] { AliasEpochDialog(block, in.index); });
            std::vector<std::string> values = in.operands;
            if (!in.value.empty()) values.insert(values.begin(), in.value);
            if (values.size() == 1) {
                std::string value = values[0];
                QObject::connect(menu->addAction(qstr("Create alias group with " + value + "...")), &QAction::triggered, [=, this] { AliasCreateDialog(block, value); });
                QObject::connect(menu->addAction(qstr("Add " + value + " to alias group...")), &QAction::triggered, [=, this] { AliasAddDialog(block, value); });
            } else if (values.size() > 1) {
                QMenu* create = menu->addMenu("Create alias group with");
                QMenu* add = menu->addMenu("Add to alias group");
                for (std::string const& value : values) {
                    QObject::connect(create->addAction(qstr(value + "...")), &QAction::triggered, [=, this] { AliasCreateDialog(block, value); });
                    QObject::connect(add->addAction(qstr(value + "...")), &QAction::triggered, [=, this] { AliasAddDialog(block, value); });
                }
            }
            std::string ir = in.ir;
            QObject::connect(menu->addAction("Copy instruction"), &QAction::triggered, [ir] { QApplication::clipboard()->setText(qstr(ir)); });
        }

        void FillBlockMenu(QMenu* menu, int block, int instr) {
            SelectBlock(block, false);
            TraceBlockView const& b = blocks[block];
            menu->addSection(QString("Block %1").arg(block));
            QObject::connect(menu->addAction(b.expanded ? "Collapse" : "Expand"), &QAction::triggered, [=, this] { Execute(std::format("{} {}", b.expanded ? "collapse" : "expand", block)); });
            QObject::connect(menu->addAction("View IR"), &QAction::triggered, [=, this] { Execute(std::format("show ir {}", block)); });
            QObject::connect(menu->addAction("View source approximation"), &QAction::triggered, [=, this] { Execute(std::format("show source {}", block)); });
            if (b.children.size() > 1) {
                QMenu* children = menu->addMenu("Show child");
                for (TraceChildView const& child : b.children) {
                    QAction* action = children->addAction(qstr(std::format("Child {}: {} [{}]", child.child, child.location, child.info)));
                    action->setCheckable(true);
                    action->setChecked(child.child == b.selected_child);
                    QObject::connect(action, &QAction::triggered, [=, this] { Execute(std::format("child {} {}", block, child.child)); });
                }
            }
            if (b.jump_to) {
                int target = *b.jump_to;
                QObject::connect(menu->addAction(QString("Go to block %1").arg(target)), &QAction::triggered, [=, this] { SelectBlock(target, true); });
            }
            if (instr >= 0) FillInstrMenu(menu, block, instr);
        }

        QToolButton* MenuButton(QToolBar* toolbar, QString const& text, std::vector<std::pair<QString, std::function<void()>>> const& actions) {
            QToolButton* button = new QToolButton;
            button->setText(text);
            button->setPopupMode(QToolButton::InstantPopup);
            QMenu* menu = new QMenu(button);
            for (auto const& [name, fn] : actions) {
                if (name.isEmpty()) menu->addSeparator();
                else QObject::connect(menu->addAction(name), &QAction::triggered, fn);
            }
            button->setMenu(menu);
            toolbar->addWidget(button);
            return button;
        }

        void BuildPage() {
            page = new QWidget;
            QVBoxLayout* layout = new QVBoxLayout(page);
            layout->setContentsMargins(0, 0, 0, 0);

            QToolBar* toolbar = new QToolBar;
            toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
            QObject::connect(toolbar->addAction("Reanalyse"), &QAction::triggered, [this] { Execute("reanalyse"); });
            toolbar->addSeparator();
            MenuButton(toolbar, "Annotate", {
                {"Function pointer targets...", [this] { FpTargetDialog(selected, SelectedInstr()); }},
                {"Allocation...", [this] { AllocDialog(selected, SelectedInstr()); }},
                {"", nullptr},
                {"Create alias group...", [this] { AliasCreateDialog(selected, ""); }},
                {"Add value to alias group...", [this] { AliasAddDialog(selected, ""); }},
                {"Set alias group type...", [this] { AliasTypeDialog(); }},
                {"Remove from alias group...", [this] { AliasRemoveDialog(); }},
                {"New alias epoch...", [this] { AliasEpochDialog(selected, SelectedInstr()); }},
                {"", nullptr},
                {"Show alias groups", [this] { Execute("alias-get"); }},
            });
            MenuButton(toolbar, "Patches", {
                {"Write annotations as patch...", [this] { FileCommand("diff", true, "Write Patch File", "annotations.patch"); }},
                {"Apply patch and reanalyse...", [this] { FileCommand("patch", false, "Read Patch File", ""); }},
                {"Dump module IR...", [this] { FileCommand("dump", true, "Dump Module", "module.ll"); }},
            });
            MenuButton(toolbar, "View", {
                {"Zoom in", [this] { view->Zoom(1.25); }},
                {"Zoom out", [this] { view->Zoom(0.8); }},
                {"Reset zoom", [this] { view->ResetZoom(); }},
                {"Fit graph", [this] { view->Fit(); }},
                {"", nullptr},
                {"Center selected block", [this] { view->centerOn(graph->Node(selected)); }},
                {"Reset graph layout", [this] { graph->ResetLayout(); Refresh(); }},
                {"", nullptr},
                {"Toggle Block IR/Source", [this] { Execute("toggle-view"); }},
            });
            QObject::connect(toolbar->addAction("Help"), &QAction::triggered, [this] { Execute("help"); });
            QWidget* spacer = new QWidget;
            spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            toolbar->addWidget(spacer);
            QObject::connect(toolbar->addAction("Exit Debugger"), &QAction::triggered, [this] { Execute("exit"); });
            layout->addWidget(toolbar);

            // Graph
            view = new GraphView(graph->Scene());
            QLabel* legend = new QLabel("Block 0 is where the trace starts; edges lead to the blocks executed before. Dashed nodes are alternative children. "
                                        "Drag nodes to move them, double-click a block to expand it, right-click for actions, Ctrl+wheel to zoom.");
            legend->setWordWrap(true);
            QWidget* graph_box = new QWidget;
            QVBoxLayout* graph_layout = new QVBoxLayout(graph_box);
            graph_layout->setContentsMargins(0, 0, 0, 0);
            graph_layout->addWidget(view, 1);
            graph_layout->addWidget(legend);

            // Block list and details
            block_list = new QTreeWidget;
            block_list->setHeaderLabels({"#", "State", "Trace"});
            block_list->setRootIsDecorated(false);
            block_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            block_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            QObject::connect(block_list, &QTreeWidget::currentItemChanged, page, [this](QTreeWidgetItem* item) {
                if (item) SelectBlock(block_list->indexOfTopLevelItem(item), true);
            });
            QObject::connect(block_list, &QTreeWidget::itemActivated, page, [this](QTreeWidgetItem* item) {
                int block = block_list->indexOfTopLevelItem(item);
                Execute(std::format("{} {}", blocks[block].expanded ? "collapse" : "expand", block));
            });

            block_title = new QLabel;
            QFont bold = block_title->font();
            bold.setBold(true);
            block_title->setFont(bold);
            block_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
            block_desc = new QLabel;
            block_desc->setWordWrap(true);
            block_desc->setTextInteractionFlags(Qt::TextSelectableByMouse);
            child_select = new QComboBox;
            child_select->setToolTip("Children are the alternative predecessors of the block's last instruction");
            QObject::connect(child_select, &QComboBox::currentIndexChanged, page, [this](int child) {
                if (child >= 0 && child != blocks[selected].selected_child) Execute(std::format("child {} {}", selected, child));
            });
            expand_button = new QPushButton;
            QObject::connect(expand_button, &QPushButton::clicked, page, [this] { Execute(std::format("{} {}", blocks[selected].expanded ? "collapse" : "expand", selected)); });
            QPushButton* view_ir = new QPushButton("View IR");
            QObject::connect(view_ir, &QPushButton::clicked, page, [this] { Execute(std::format("show ir {}", selected)); });
            QPushButton* view_source = new QPushButton("View Source");
            QObject::connect(view_source, &QPushButton::clicked, page, [this] { Execute(std::format("show source {}", selected)); });
            QHBoxLayout* block_buttons = new QHBoxLayout;
            block_buttons->addWidget(new QLabel("Child:"));
            block_buttons->addWidget(child_select, 1);
            block_buttons->addWidget(expand_button);
            block_buttons->addWidget(view_ir);
            block_buttons->addWidget(view_source);

            instr_table = new QTableWidget(0, 4);
            instr_table->setHorizontalHeaderLabels({"Instr", "State", "IR", "Location"});
            instr_table->verticalHeader()->hide();
            instr_table->verticalHeader()->setDefaultSectionSize(instr_table->fontMetrics().height() + 6);
            instr_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            instr_table->setSelectionMode(QAbstractItemView::SingleSelection);
            instr_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            instr_table->setWordWrap(false);
            instr_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            instr_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            instr_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
            instr_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
            instr_table->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(instr_table, &QTableWidget::customContextMenuRequested, page, [this](QPoint const& pos) {
                int row = instr_table->rowAt(pos.y());
                if (row < 0) return;
                QMenu menu;
                FillInstrMenu(&menu, selected, row);
                menu.exec(instr_table->viewport()->mapToGlobal(pos));
            });
            source_text = new QPlainTextEdit;
            source_text->setReadOnly(true);
            source_text->setLineWrapMode(QPlainTextEdit::NoWrap);
            source_text->setFont(GUIManager::MonoFont());
            QTabWidget* detail_tabs = new QTabWidget;
            detail_tabs->addTab(instr_table, "Instructions");
            detail_tabs->addTab(source_text, "Source Approx.");

            QWidget* details = new QWidget;
            QVBoxLayout* details_layout = new QVBoxLayout(details);
            details_layout->setContentsMargins(0, 0, 0, 0);
            details_layout->addWidget(block_title);
            details_layout->addWidget(block_desc);
            details_layout->addLayout(block_buttons);
            details_layout->addWidget(detail_tabs, 1);

            QSplitter* side = new QSplitter(Qt::Vertical);
            side->addWidget(block_list);
            side->addWidget(details);
            side->setStretchFactor(1, 2);

            QSplitter* top = new QSplitter(Qt::Horizontal);
            top->addWidget(graph_box);
            top->addWidget(side);
            top->setStretchFactor(0, 3);
            top->setStretchFactor(1, 2);

            // Command output, history and input
            output = new QPlainTextEdit;
            output->setReadOnly(true);
            output->setFont(GUIManager::MonoFont());
            history_table = new QTableWidget(0, 3);
            history_table->setHorizontalHeaderLabels({"#", "Return Code", "Command"});
            history_table->verticalHeader()->hide();
            history_table->verticalHeader()->setDefaultSectionSize(history_table->fontMetrics().height() + 6);
            history_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
            history_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
            history_table->horizontalHeader()->setStretchLastSection(true);
            history_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            history_table->setSelectionBehavior(QAbstractItemView::SelectRows);
            QObject::connect(history_table, &QTableWidget::cellDoubleClicked, page, [this](int row) { console->setText(qstr(history[row].first)); console->setFocus(); });
            bottom_tabs = new QTabWidget;
            bottom_tabs->addTab(output, "Output");
            bottom_tabs->addTab(history_table, "History");

            console = new CommandLine(history);
            console->setPlaceholderText("Type \"help\" for help");
            console->setFont(GUIManager::MonoFont());
            QStringList names;
            for (CmdInfo const& cmd : commands) names << qstr(std::string(cmd.name));
            QCompleter* completer = new QCompleter(names, console);
            completer->setCompletionMode(QCompleter::InlineCompletion);
            console->setCompleter(completer);
            last_result = new QLabel;
            last_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
            QLabel* usage = new QLabel;
            QObject::connect(console, &QLineEdit::textChanged, page, [this, usage](QString const& text) {
                std::string name = text.section(' ', 0, 0).toStdString();
                std::vector<CmdInfo>::const_iterator it = std::ranges::find_if(commands, [&](CmdInfo const& cmd) { return cmd.name == name; });
                usage->setText(it == commands.end() ? "" : qstr(std::format("Usage: {} {} - {}", it->name, it->usage, it->helptext)));
            });
            QObject::connect(console, &QLineEdit::returnPressed, page, [this] {
                std::string cmd = console->text().toStdString();
                console->clear();
                Execute(cmd);
            });
            QHBoxLayout* input = new QHBoxLayout;
            input->addWidget(new QLabel(">>>"));
            input->addWidget(console, 1);

            QWidget* bottom = new QWidget;
            QVBoxLayout* bottom_layout = new QVBoxLayout(bottom);
            bottom_layout->setContentsMargins(0, 0, 0, 0);
            bottom_layout->addWidget(bottom_tabs, 1);
            bottom_layout->addWidget(last_result);
            bottom_layout->addLayout(input);
            bottom_layout->addWidget(usage);

            QSplitter* main = new QSplitter(Qt::Vertical);
            main->addWidget(top);
            main->addWidget(bottom);
            main->setStretchFactor(0, 4);
            main->setStretchFactor(1, 1);
            layout->addWidget(main, 1);
        }
};

export bool ShowTrace(TraceSession& session) {
    GUIManager::Window(); // Sets up the application
    TraceDebugger debugger(session);
    return debugger.Exec();
}

} // namespace GUITrace
