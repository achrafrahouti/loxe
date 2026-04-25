#pragma once

#include <QAbstractItemModel>
#include <QString>
#include <cstdint>
#include <vector>

struct TreeNode {
    uint64_t         byteOffset  = 0;
    int              depth       = 0;
    QString          tagName;
    QString          attrSummary;  // [@attr="val"] shown in Attributes column
    QString          textPreview;  // ≤60 chars text content for Text column
    bool             hasChildren = false;
    bool             isLoaded    = false;
    int              parentIndex = -1;
    int              firstChild  = -1;
    int              nextSibling = -1;
    std::vector<int> navPath;      // sibling indices from XML root to reach this node
};

// QAbstractItemModel backed by a flat std::vector<TreeNode>.
// Each node stores only (byteOffset, depth, tagName) plus navigation links.
// Children are loaded lazily via fetchMore() using CMarkup streaming on demand.
class VirtualTreeModel : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit VirtualTreeModel(const QString& filePath, QObject* parent = nullptr);

    // Append root-level nodes (called from AsyncLoader Phase 3).
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
    // Open a fresh CMarkup, navigate to nodeIndex's path, populate its children.
    void loadChildren(int nodeIndex);

    // Returns the node index encoded in a QModelIndex internalId.
    int nodeIndexOf(const QModelIndex& idx) const;

    // Returns the row of nodeIdx among its siblings (O(siblings)).
    int rowOfNode(int nodeIdx) const;

    QString               m_filePath;
    std::vector<TreeNode> m_nodes;
};
