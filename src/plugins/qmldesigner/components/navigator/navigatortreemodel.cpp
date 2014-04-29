/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "navigatortreemodel.h"

#include <nodeabstractproperty.h>
#include <nodelistproperty.h>
#include <nodeproperty.h>
#include <variantproperty.h>
#include <metainfo.h>
#include <abstractview.h>
#include <rewriterview.h>
#include <invalididexception.h>
#include <rewritingexception.h>
#include <modelnodecontextmenu.h>
#include <qmlitemnode.h>

#include <coreplugin/icore.h>

#include <QMimeData>
#include <QMessageBox>
#include <QApplication>
#include <QPointF>

namespace QmlDesigner {

static PropertyNameList visibleProperties(const ModelNode &node)
{
    PropertyNameList propertyList;

    foreach (const PropertyName &propertyName, node.metaInfo().propertyNames()) {
        if (!propertyName.contains('.') //do not show any dot properties, since they are tricky and unlikely to make sense
                && node.metaInfo().propertyIsWritable(propertyName)
                && propertyName != "parent"
                && node.metaInfo().propertyTypeName(propertyName) != TypeName("Component")
                && !node.metaInfo().propertyIsEnumType(propertyName) //Some enums have the same name as Qml types (e. g. Flow)
                && !node.metaInfo().propertyIsPrivate(propertyName) //Do not show private properties
                && propertyName != node.metaInfo().defaultPropertyName()) { // TODO: ask the node instances

            TypeName qmlType = node.metaInfo().propertyTypeName(propertyName);
            if (node.model()->metaInfo(qmlType).isValid() &&
                node.model()->metaInfo(qmlType).isSubclassOf("QtQuick.Item", -1, -1)) {
                propertyList.append(propertyName);
            }
        }
    }

    return propertyList;
}

static QList<ModelNode> acceptedModelNodeChildren(const ModelNode &parentNode)
{
    QList<ModelNode> children;
    PropertyNameList properties;

    if (parentNode.metaInfo().hasDefaultProperty())
        properties.append(parentNode.metaInfo().defaultPropertyName());

    properties.append(visibleProperties(parentNode));

    foreach (const PropertyName &propertyName, properties) {
        AbstractProperty property(parentNode.property(propertyName));
        if (property.isNodeAbstractProperty())
            children.append(property.toNodeAbstractProperty().directSubNodes());
    }

    return children;
}

NavigatorTreeModel::NavigatorTreeModel(QObject *parent)
    : QStandardItemModel(parent),
      m_blockItemChangedSignal(false)
{
    invisibleRootItem()->setFlags(Qt::ItemIsDropEnabled);

#    ifdef _LOCK_ITEMS_
    setColumnCount(3);
#    else
    setColumnCount(2);
#    endif

    setSupportedDragActions(Qt::LinkAction);

    connect(this, SIGNAL(itemChanged(QStandardItem*)),
            this, SLOT(handleChangedItem(QStandardItem*)));
}

NavigatorTreeModel::~NavigatorTreeModel()
{
}

Qt::DropActions NavigatorTreeModel::supportedDropActions() const
{
    return Qt::LinkAction;
}

QStringList NavigatorTreeModel::mimeTypes() const
{
     QStringList types;
     types.append("application/vnd.modelnode.list");
     return types;
}

QMimeData *NavigatorTreeModel::mimeData(const QModelIndexList &indexList) const
{
     QMimeData *mimeData = new QMimeData();
     QByteArray encodedData;

     QSet<QModelIndex> rowAlreadyUsedSet;

     QDataStream stream(&encodedData, QIODevice::WriteOnly);

     foreach (const QModelIndex &index, indexList) {
         if (!index.isValid())
             continue;
         QModelIndex idIndex = index.sibling(index.row(), 0);
         if (rowAlreadyUsedSet.contains(idIndex))
             continue;

         rowAlreadyUsedSet.insert(idIndex);
         stream << idIndex.data(InternalIdRole).toInt();
     }

     mimeData->setData("application/vnd.modelnode.list", encodedData);

     return mimeData;
}

bool NavigatorTreeModel::dropMimeData(const QMimeData *data,
                  Qt::DropAction action,
                  int row,
                  int column,
                  const QModelIndex &dropIndex)
{
    if (action == Qt::IgnoreAction)
        return true;
    if (action != Qt::LinkAction)
        return false;
    if (!data->hasFormat("application/vnd.modelnode.list"))
        return false;
    if (column > 1)
        return false;
    if (dropIndex.model() != this)
        return false;

    QModelIndex parentIndex, parentItemIndex;
    PropertyName parentPropertyName;
    int targetIndex;

    parentIndex = dropIndex.sibling(dropIndex.row(), 0);
    targetIndex = (row > -1)? row : rowCount(parentIndex);

    if (this->data(parentIndex, InternalIdRole).isValid()) {
        parentItemIndex = parentIndex;
        ModelNode parentNode = nodeForIndex(parentItemIndex);
        if (!parentNode.metaInfo().hasDefaultProperty())
            return false;
        targetIndex -= visibleProperties(parentNode).count();
        parentPropertyName = parentNode.metaInfo().defaultPropertyName();
    } else {
        parentItemIndex = parentIndex.parent();
        parentPropertyName = parentIndex.data(Qt::DisplayRole).toByteArray();
    }

    // Disallow dropping items between properties, which are listed first.
    if (targetIndex < 0)
        return false;

    Q_ASSERT(parentItemIndex.isValid());

    QByteArray encodedData = data->data("application/vnd.modelnode.list");
    QDataStream stream(&encodedData, QIODevice::ReadOnly);

    QList<ModelNode> nodeList;
    while (!stream.atEnd()) {
        qint32 internalId;
        stream >> internalId;
        if (m_view->hasModelNodeForInternalId(internalId))
            nodeList.append(m_view->modelNodeForInternalId(internalId));
    }

    ModelNode parentNode(nodeForIndex(parentItemIndex));
    NodeAbstractProperty parentProperty = parentNode.nodeAbstractProperty(parentPropertyName);

    if (parentProperty.isNodeProperty() &&
        nodeList.count() > 1) {
        return false;
    }

    moveNodesInteractive(parentProperty, nodeList, targetIndex);

    return false; // don't let the view do drag&drop on its own
}

static inline QString msgUnknownItem(const QString &t)
{
    return NavigatorTreeModel::tr("Unknown item: %1").arg(t);
}

ItemRow NavigatorTreeModel::createItemRow(const ModelNode &node)
{
    Q_ASSERT(node.isValid());

    const bool dropEnabled = node.metaInfo().isValid();

    QStandardItem *idItem = new QStandardItem;
    idItem->setDragEnabled(true);
    idItem->setDropEnabled(dropEnabled);
    idItem->setEditable(true);
    idItem->setData(node.internalId(), InternalIdRole);
    if (node.metaInfo().isValid())
        idItem->setToolTip(node.type());
    else
        idItem->setToolTip(msgUnknownItem(node.type()));
#    ifdef _LOCK_ITEMS_
    QStandardItem *lockItem = new QStandardItem;
    lockItem->setDragEnabled(true);
    lockItem->setDropEnabled(dropEnabled);
    lockItem->setEditable(false);
    lockItem->setCheckable(true);
    lockItem->setData(hash, NavigatorRole);
#    endif

    QStandardItem *visibilityItem = new QStandardItem;
    visibilityItem->setDropEnabled(dropEnabled);
    visibilityItem->setCheckable(true);
    visibilityItem->setEditable(false);
    visibilityItem->setData(node.internalId(), InternalIdRole);
    if (node.isRootNode())
        visibilityItem->setCheckable(false);

    QMap<QString, QStandardItem *> propertyItems;
    foreach (const QString &propertyName, visibleProperties(node)) {
        QStandardItem *propertyItem = new QStandardItem;
        propertyItem->setSelectable(false);
        propertyItem->setDragEnabled(false);
        propertyItem->setDropEnabled(dropEnabled);
        propertyItem->setEditable(false);
        propertyItem->setData(propertyName, Qt::DisplayRole);
        propertyItems.insert(propertyName, propertyItem);
        idItem->appendRow(propertyItem);
    }

#   ifdef _LOCK_ITEMS_
    ItemRow newRow =  ItemRow(idItem, lockItem, visibilityItem, propertyItems);
#   else
    ItemRow newRow = ItemRow(idItem, visibilityItem, propertyItems);
#   endif

    m_nodeItemHash.insert(node, newRow);
    updateItemRow(node, newRow);

    return newRow;
}

void NavigatorTreeModel::updateItemRow(const ModelNode &node, ItemRow items)
{
    bool blockSignal = blockItemChangedSignal(true);

    items.idItem->setText(node.id());
    items.visibilityItem->setCheckState(node.auxiliaryData("invisible").toBool() ? Qt::Unchecked : Qt::Checked);
    if (node.metaInfo().isValid())
        items.idItem->setToolTip(node.type());
    else
        items.idItem->setToolTip(msgUnknownItem(node.type()));

    blockItemChangedSignal(blockSignal);
}

/**
  Update the information shown for a node / property
  */
void NavigatorTreeModel::updateItemRow(const ModelNode &node)
{
    if (!isInTree(node))
        return;

    updateItemRow(node, itemRowForNode(node));
}

/**
  Updates the sibling position of the item, depending on the position in the model.
  */
void NavigatorTreeModel::updateItemRowOrder(const NodeListProperty &listProperty, const ModelNode &modelNode, int /*oldIndex*/)
{
    if (isInTree(modelNode)) {
        ItemRow itemRow = itemRowForNode(modelNode);
        int currentRowIndex = itemRow.idItem->row();
        int newRowIndex = listProperty.indexOf(modelNode);

        QStandardItem *targetItem = 0;
        if (isInTree(listProperty.parentModelNode())) {
            ItemRow parentRow = itemRowForNode(listProperty.parentModelNode());
            if (parentRow.propertyItems.contains(listProperty.name())) {
                targetItem = parentRow.propertyItems.value(listProperty.name());
            } else  { // default property
                targetItem = parentRow.idItem;
                newRowIndex += visibleProperties(listProperty.parentModelNode()).count();
            }
        } else {
            targetItem = itemRow.idItem->parent();
        }

        if (targetItem && currentRowIndex != newRowIndex) {//### Items without a parent should not exist
            QList<QStandardItem*> items = targetItem->takeRow(currentRowIndex);
            targetItem->insertRow(newRowIndex, items);
        }
    }
}

static void handleWrongId(QStandardItem *item, const ModelNode &modelNode, const QString &errorTitle, const QString &errorMessage, NavigatorTreeModel *treeModel)
{
    QMessageBox::warning(Core::ICore::dialogParent(), errorTitle,  errorMessage);
    bool blockSingals = treeModel->blockItemChangedSignal(true);
    item->setText(modelNode.id());
    treeModel->blockItemChangedSignal(blockSingals);
}


void NavigatorTreeModel::handleChangedIdItem(QStandardItem *idItem, ModelNode &modelNode)
{
    const QString newId = idItem->text();
    if (!modelNode.isValidId(newId)) {
        handleWrongId(idItem, modelNode, tr("Invalid Id"), tr("%1 is an invalid id.").arg(newId), this);
    } else if (modelNode.view()->hasId(newId)) {
        handleWrongId(idItem, modelNode, tr("Invalid Id"), tr("%1 already exists.").arg(newId), this);
    } else  {
        modelNode.setIdWithRefactoring(newId);
    }
}

void NavigatorTreeModel::handleChangedVisibilityItem(QStandardItem *visibilityItem, ModelNode &modelNode)
{
    bool invisible = (visibilityItem->checkState() == Qt::Unchecked);

    if (invisible)
        modelNode.setAuxiliaryData("invisible", invisible);
    else
        modelNode.removeAuxiliaryData("invisible");
}

void NavigatorTreeModel::handleChangedItem(QStandardItem *item)
{
    QVariant internalIdVariant = data(item->index(), InternalIdRole);
    if (!m_blockItemChangedSignal && internalIdVariant.isValid()) {
        ModelNode modelNode = m_view->modelNodeForInternalId(internalIdVariant.toInt());
        ItemRow itemRow = itemRowForNode(modelNode);
        if (item == itemRow.idItem) {
            handleChangedIdItem(item, modelNode);
        } else if (item == itemRow.visibilityItem) {
            handleChangedVisibilityItem(item, modelNode);
        }
    }
}

ItemRow NavigatorTreeModel::itemRowForNode(const ModelNode &node)
{
    Q_ASSERT(node.isValid());
    return m_nodeItemHash.value(node);
}

void NavigatorTreeModel::setView(AbstractView *view)
{
    m_view = view;
    if (view)
        addSubTree(view->rootModelNode());
}

void NavigatorTreeModel::clearView()
{
    setView(0);
    m_view.clear();
    m_nodeItemHash.clear();
}

QModelIndex NavigatorTreeModel::indexForNode(const ModelNode &node) const
{
    Q_ASSERT(node.isValid());
    if (!isInTree(node))
        return QModelIndex();
    ItemRow row = m_nodeItemHash.value(node);
    return row.idItem->index();
}

ModelNode NavigatorTreeModel::nodeForIndex(const QModelIndex &index) const
{
    qint32 internalId = index.data(InternalIdRole).toInt();
    return m_view->modelNodeForInternalId(internalId);
}

bool NavigatorTreeModel::isInTree(const ModelNode &node) const
{
    return m_nodeItemHash.contains(node);
}

bool NavigatorTreeModel::isNodeInvisible(const QModelIndex &index) const
{
    return isNodeInvisible(nodeForIndex(index));
}

static bool isInvisbleInHierachy(const ModelNode &modelNode)
{
    if (modelNode.auxiliaryData("invisible").toBool())
        return true;

    if (modelNode.hasParentProperty())
        return isInvisbleInHierachy(modelNode.parentProperty().parentModelNode());

    return false;
}

bool NavigatorTreeModel::isNodeInvisible(const ModelNode &modelNode) const
{
    return isInvisbleInHierachy(modelNode);
}

static bool isRootNodeOrAcceptedChild(const ModelNode &modelNode)
{
    return modelNode.isRootNode() || acceptedModelNodeChildren(modelNode.parentProperty().parentModelNode()).contains(modelNode);
}

static bool nodeCanBeHandled(const ModelNode &modelNode)
{
    return modelNode.metaInfo().isGraphicalItem() && isRootNodeOrAcceptedChild(modelNode);
}

static void appendNodeToEndOfTheRow(const ModelNode &modelNode, const ItemRow &newItemRow, NavigatorTreeModel *treeModel)
{
    if (modelNode.hasParentProperty()) {
        NodeAbstractProperty parentProperty(modelNode.parentProperty());
        ItemRow parentRow = treeModel->itemRowForNode(parentProperty.parentModelNode());
        if (parentRow.propertyItems.contains(parentProperty.name())) {
            QStandardItem *parentPropertyItem = parentRow.propertyItems.value(parentProperty.name());
            parentPropertyItem->appendRow(newItemRow.toList());
        } else {
            QStandardItem *parentDefaultPropertyItem = parentRow.idItem;
            parentDefaultPropertyItem->appendRow(newItemRow.toList());
        }
    } else { // root node
        treeModel->appendRow(newItemRow.toList());
    }
}

void NavigatorTreeModel::addSubTree(const ModelNode &modelNode)
{
    if (nodeCanBeHandled(modelNode)) {

        ItemRow newItemRow = createItemRow(modelNode);

        foreach (const ModelNode &childNode, acceptedModelNodeChildren(modelNode))
            addSubTree(childNode);

        appendNodeToEndOfTheRow(modelNode, newItemRow, this);
    }
}

static QList<QStandardItem*> takeWholeRow(const ItemRow &itemRow)
{
    if (itemRow.idItem->parent())
        return  itemRow.idItem->parent()->takeRow(itemRow.idItem->row());
    else
        return itemRow.idItem->model()->takeRow(itemRow.idItem->row());
}

void NavigatorTreeModel::removeSubTree(const ModelNode &node)
{
    if (isInTree(node)) {
        ItemRow itemRow = itemRowForNode(node);

        QList<QStandardItem*> rowList = takeWholeRow(itemRow);

        foreach (const ModelNode &childNode, acceptedModelNodeChildren(node))
            removeSubTree(childNode);

        qDeleteAll(rowList);
        m_nodeItemHash.remove(node);
    }

}

static void removePosition(const ModelNode &node)
{
    ModelNode modelNode = node;
    if (modelNode.hasProperty("x"))
        modelNode.removeProperty("x");
    if (modelNode.hasProperty("y"))
        modelNode.removeProperty("y");
}

static void setScenePosition(const QmlDesigner::ModelNode &modelNode,const QPointF &positionInSceneSpace)
{
    if (modelNode.hasParentProperty() && QmlDesigner::QmlItemNode::isValidQmlItemNode(modelNode.parentProperty().parentModelNode())) {
        QmlDesigner::QmlItemNode parentNode = modelNode.parentProperty().parentQmlObjectNode().toQmlItemNode();
        QPointF positionInLocalSpace = parentNode.instanceSceneContentItemTransform().inverted().map(positionInSceneSpace);
        modelNode.variantProperty("x").setValue(positionInLocalSpace.toPoint().x());
        modelNode.variantProperty("y").setValue(positionInLocalSpace.toPoint().y());
    }
}

void NavigatorTreeModel::moveNodesInteractive(NodeAbstractProperty parentProperty, const QList<ModelNode> &modelNodes, int targetIndex)
{
    try {
        TypeName propertyQmlType = parentProperty.parentModelNode().metaInfo().propertyTypeName(parentProperty.name());

        RewriterTransaction transaction = m_view->beginRewriterTransaction(QByteArrayLiteral("NavigatorTreeModel::moveNodesInteractive"));
        foreach (const ModelNode &node, modelNodes) {
            if (!node.isValid())
                continue;

            if (node != parentProperty.parentModelNode() &&
                !node.isAncestorOf(parentProperty.parentModelNode()) &&
                (node.metaInfo().isSubclassOf(propertyQmlType, -1, -1) || propertyQmlType == "alias")) {
                    //### todo: allowing alias is just a heuristic
                    //once the MetaInfo is part of instances we can do this right

                    if (node.parentProperty() != parentProperty) {

                        if (parentProperty.isNodeProperty()) {
                            ModelNode propertyNode = parentProperty.toNodeProperty().modelNode();
                            // Destruction of ancestors is not allowed
                            if (propertyNode.isAncestorOf(node))
                                continue;
                            if (propertyNode.isValid()) {
                                QApplication::setOverrideCursor(Qt::ArrowCursor);
                                if (QMessageBox::warning(Core::ICore::dialogParent(), tr("Warning"),
                                                         tr("Reparenting the component %1 here will cause the "
                                                            "component %2 to be deleted. Do you want to proceed?")
                                                            .arg(node.id(), propertyNode.id()),
                                                         QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Cancel) {
                                    QApplication::restoreOverrideCursor();
                                    continue;
                                }
                                QApplication::restoreOverrideCursor();
                                propertyNode.destroy();
                            }
                        }

                        if (parentProperty.isDefaultProperty() && parentProperty.parentModelNode().metaInfo().isLayoutable()) {
                             removePosition(node);
                             parentProperty.reparentHere(node);
                        } else {
                            if (QmlItemNode::isValidQmlItemNode(node)) {
                                QPointF scenePosition = QmlItemNode(node).instanceScenePosition();
                                parentProperty.reparentHere(node);
                                if (!scenePosition.isNull())
                                    setScenePosition(node, scenePosition);
                            } else {
                                parentProperty.reparentHere(node);
                            }
                        }
                    }

                    if (parentProperty.isNodeListProperty()) {
                        int index = parentProperty.indexOf(node);
                        if (index < targetIndex) { // item is first removed from oldIndex, then inserted at new index
                            --targetIndex;
                        }
                        if (index != targetIndex)
                            parentProperty.toNodeListProperty().slide(index, targetIndex);
                    }
            }
        }
    }  catch (RewritingException &exception) { //better safe than sorry! There always might be cases where we fail
        exception.showException();
    }
}

// along the lines of QObject::blockSignals
bool NavigatorTreeModel::blockItemChangedSignal(bool block)
{
    bool oldValue = m_blockItemChangedSignal;
    m_blockItemChangedSignal = block;
    return oldValue;
}

void NavigatorTreeModel::setId(const QModelIndex &index, const QString &id)
{
    ModelNode node = nodeForIndex(index);
    ItemRow itemRow = itemRowForNode(node);
    itemRow.idItem->setText(id);
}

void NavigatorTreeModel::setVisible(const QModelIndex &index, bool visible)
{
    ModelNode node = nodeForIndex(index);
    ItemRow itemRow = itemRowForNode(node);
    itemRow.visibilityItem->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
}

void NavigatorTreeModel::openContextMenu(const QPoint &position)
{
    ModelNodeContextMenu::showContextMenu(m_view.data(), position, QPoint(), false);
}

} // QmlDesigner
