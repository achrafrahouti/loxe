#pragma once

#include <QAbstractItemModel>
#include <QString>
#include <cstdint>
#include <vector>

class MmapBuffer;

struct TreeNode {
    uint64_t byteOffset  = 0;
    int      depth       = 0;
    QString  tagName;
    bool     hasChildren = false;
    bool     isLoaded    = false; // true once children have been fetched
    int      parentIndex = -1;   // index into m_nodes; -1 for root items
    int      firstChild  = -1;   // index of first child node, -1 if none loaded
    int      nextSibling = -1;   // index of next sibling, -1 if last
};

// QAbstractItemModel backed by an array of (byte_offset, depth, tag_name) tuples.
// Stores NO full node content — child data is loaded lazily via fetchMore()
// by seeking CMarkup to the stored byte_offset.
class VirtualTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit VirtualTreeModel(MmapBuffer* buf, QObject* parent = nullptr);

    // Append root-level nodes from the level-1 background parse.
    void appendRootNodes(std::vector<TreeNode> nodes);

    // QAbstractItemModel interface
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int         rowCount(const QModelIndex& parent = {}) const override;
    int         columnCount(const QModelIndex& parent = {}) const override;
    QVariant    data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant    headerData(int section, Qt::Orientation orientation,
                           int role = Qt::DisplayRole) const override;
    bool        hasChildren(const QModelIndex& parent = {}) const override;
    bool        canFetchMore(const QModelIndex& parent) const override;
    void        fetchMore(const QModelIndex& parent) override;

    // Returns the byte offset for the node at index (used by ViewportRenderer sync).
    uint64_t byteOffsetFor(const QModelIndex& index) const;

private:
    // Parse children of the node at nodeIndex using CMarkup positioned at its byteOffset.
    void loadChildren(int nodeIndex);

    // Returns the node index stored in a QModelIndex (encoded as internalId).
    int nodeIndexOf(const QModelIndex& idx) const;

    MmapBuffer*           m_buf;
    std::vector<TreeNode> m_nodes;
};
