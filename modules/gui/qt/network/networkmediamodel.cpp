/*****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "networkmediamodel.hpp"

#include "medialibrary/mlhelper.hpp"

#include "playlist/media.hpp"
#include "playlist/playlist_controller.hpp"

namespace {

enum Role {
    NETWORK_NAME = Qt::UserRole + 1,
    NETWORK_MRL,
    NETWORK_INDEXED,
    NETWORK_CANINDEX,
    NETWORK_TYPE,
    NETWORK_PROTOCOL,
    NETWORK_TREE,
    NETWORK_SOURCE,
};

}

NetworkMediaModel::NetworkMediaModel( QObject* parent )
    : QAbstractListModel( parent )
    , m_ml( nullptr )
{
}

QVariant NetworkMediaModel::data( const QModelIndex& index, int role ) const
{
    if (!m_ctx)
        return {};
    auto idx = index.row();
    if ( idx < 0 || (size_t)idx >= m_items.size() )
        return {};
    const auto& item = m_items[idx];
    switch ( role )
    {
        case NETWORK_NAME:
            return item.name;
        case NETWORK_MRL:
            return item.mainMrl;
        case NETWORK_INDEXED:
            return item.indexed;
        case NETWORK_CANINDEX:
            return item.canBeIndexed;
        case NETWORK_TYPE:
            return item.type;
        case NETWORK_PROTOCOL:
            return item.protocol;
        case NETWORK_TREE:
            return QVariant::fromValue( item.tree );
        case NETWORK_SOURCE:
            return item.mediaSource->description;
        default:
            return {};
    }
}

QHash<int, QByteArray> NetworkMediaModel::roleNames() const
{
    return {
        { NETWORK_NAME, "name" },
        { NETWORK_MRL, "mrl" },
        { NETWORK_INDEXED, "indexed" },
        { NETWORK_CANINDEX, "can_index" },
        { NETWORK_TYPE, "type" },
        { NETWORK_PROTOCOL, "protocol" },
        { NETWORK_TREE, "tree" },
        { NETWORK_SOURCE, "source" },
    };
}

int NetworkMediaModel::rowCount(const QModelIndex& parent) const
{
    if ( parent.isValid() )
        return 0;
    assert( m_items.size() < INT32_MAX );
    return static_cast<int>( m_items.size() );
}

Qt::ItemFlags NetworkMediaModel::flags( const QModelIndex& idx ) const
{
    return QAbstractListModel::flags( idx ) | Qt::ItemIsEditable;
}

bool NetworkMediaModel::setData( const QModelIndex& idx, const QVariant& value, int role )
{
    if (!m_ml)
        return false;

    if ( role != NETWORK_INDEXED )
        return false;
    auto enabled = value.toBool();
    if ( m_items[idx.row()].indexed == enabled )
        return  false;
    int res;
    if ( enabled )
        res = vlc_ml_add_folder( m_ml, qtu( m_items[idx.row()].mainMrl.toString( QUrl::None ) ) );
    else
        res = vlc_ml_remove_folder( m_ml, qtu( m_items[idx.row()].mainMrl.toString( QUrl::None ) ) );
    m_items[idx.row()].indexed = enabled;
    emit dataChanged(idx, idx, { NETWORK_INDEXED });
    return res == VLC_SUCCESS;
}


void NetworkMediaModel::setIndexed(bool indexed)
{
    if (indexed == m_indexed || !m_canBeIndexed)
        return;
    int res;
    if ( indexed ) {
        res = vlc_ml_add_folder( m_ml, qtu( m_url.toString( QUrl::FullyEncoded ) ) );
    } else
        res = vlc_ml_remove_folder( m_ml, qtu( m_url.toString( QUrl::FullyEncoded ) ) );

    if (res == VLC_SUCCESS) {
        m_indexed = indexed;
        emit isIndexedChanged();
    }
}

void NetworkMediaModel::setCtx(QmlMainContext* ctx)
{
    if (ctx) {
        m_ctx = ctx;
        m_ml = vlc_ml_instance_get( m_ctx->getIntf() );
    }
    if (m_ctx && m_hasTree) {
        initializeMediaSources();
    }
    emit ctxChanged();
}

void NetworkMediaModel::setTree(QVariant parentTree)
{
    if (parentTree.canConvert<NetworkTreeItem>())
        m_treeItem = parentTree.value<NetworkTreeItem>();
    else
        m_treeItem = NetworkTreeItem();
    m_hasTree = true;
    if (m_ctx && m_hasTree) {
        initializeMediaSources();
    }
    emit treeChanged();
}

bool NetworkMediaModel::addToPlaylist(int index)
{
    if (!(m_ctx && m_hasTree))
        return false;
    if (index < 0 || (size_t)index >= m_items.size() )
        return false;
    auto item =  m_items[index];
    vlc::playlist::Media media{ item.tree.media.get() };
    m_ctx->getIntf()->p_sys->p_mainPlaylistController->append( { media }, false);
    return true;
}

bool NetworkMediaModel::addToPlaylist(const QVariantList &itemIdList)
{
    bool ret = false;
    for (const QVariant& varValue: itemIdList)
    {
        if (varValue.canConvert<int>())
        {
            auto index = varValue.value<int>();
            ret |= addToPlaylist(index);
        }
    }
    return ret;
}

bool NetworkMediaModel::addAndPlay(int index)
{
    if (!(m_ctx && m_hasTree))
        return false;
    if (index < 0 || (size_t)index >= m_items.size() )
        return false;
    auto item =  m_items[index];
    vlc::playlist::Media media{ item.tree.media.get() };
    m_ctx->getIntf()->p_sys->p_mainPlaylistController->append( { media }, true);
    return true;
}

bool NetworkMediaModel::addAndPlay(const QVariantList& itemIdList)
{
    bool ret = false;
    for (const QVariant& varValue: itemIdList)
    {
        if (varValue.canConvert<int>())
        {
            auto index = varValue.value<int>();
            if (!ret)
                ret |= addAndPlay(index);
            else
                ret |= addToPlaylist(index);
        }
    }
    return ret;
}


bool NetworkMediaModel::initializeMediaSources()
{
    auto libvlc = vlc_object_instance(m_ctx->getIntf());

    m_listener.reset();
    if (!m_items.empty()) {
        beginResetModel();
        m_items.clear();
        endResetModel();
    }

    if (!m_treeItem)
        return false;

    auto tree = m_treeItem.source->tree;
    auto l = std::make_unique<NetworkSourceListener>( m_treeItem.source, this );
    if ( l->listener == nullptr )
        return false;

    if (m_treeItem.media)
    {
        m_name = m_treeItem.media->psz_name;
        emit nameChanged();
        m_url = QUrl::fromEncoded( QByteArray{ m_treeItem.media->psz_uri }.append( '/' ) );
        emit urlChanged();
        m_type = static_cast<ItemType>(m_treeItem.media->i_type);
        emit typeChanged();
        m_canBeIndexed = canBeIndexed( m_url, m_type );
        emit canBeIndexedChanged();
        if ( vlc_ml_is_indexed( m_ml, QByteArray(m_treeItem.media->psz_uri).append('/').constData(), &m_indexed ) != VLC_SUCCESS ) {
            m_indexed = false;
        }
        emit isIndexedChanged();
    }


    vlc_media_tree_Preparse( tree, libvlc, m_treeItem.media.get() );
    m_parsingPending = true;
    emit parsingPendingChanged(m_parsingPending);

    m_listener = std::move( l );

    return true;
}

void NetworkMediaModel::onItemCleared( MediaSourcePtr mediaSource, input_item_node_t*)
{
    input_item_node_t *res;
    input_item_node_t *parent;
    if ( vlc_media_tree_Find( m_treeItem.source->tree, m_treeItem.media.get(),
                                      &res, &parent ) == false )
        return;
    refreshMediaList( std::move( mediaSource ), res->pp_children, res->i_children, true );
}

void NetworkMediaModel::onItemAdded( MediaSourcePtr mediaSource, input_item_node_t* parent,
                                  input_item_node_t *const children[],
                                  size_t count )
{
    if ( parent->p_item == m_treeItem.media.get() )
        refreshMediaList( std::move( mediaSource ), children, count, false );
}

void NetworkMediaModel::onItemRemoved( MediaSourcePtr,
                                    input_item_node_t *const children[],
                                    size_t count )
{
    for ( auto i = 0u; i < count; ++i )
    {
        InputItemPtr p_item { children[i]->p_item };
        QMetaObject::invokeMethod(this, [this, p_item=std::move(p_item)]() {
            QUrl itemUri = QUrl::fromEncoded(p_item->psz_uri);
            auto it = std::find_if( begin( m_items ), end( m_items ), [p_item, itemUri](const Item& i) {
                return QString::compare( qfu(p_item->psz_name), i.name, Qt::CaseInsensitive ) == 0 &&
                    itemUri.scheme() == i.mainMrl.scheme();
            });
            if ( it == end( m_items ) )
                return;

            auto mrlIt = std::find_if( begin( (*it).mrls ), end( (*it).mrls),
                                       [itemUri]( const QUrl& mrl ) {
                return mrl == itemUri;
            });
            if ( mrlIt == end( (*it).mrls ) )
                return;
            (*it).mrls.erase( mrlIt );
            if ( (*it).mrls.empty() == false )
                return;
            auto idx = std::distance( begin( m_items ), it );
            beginRemoveRows({}, idx, idx );
            m_items.erase( it );
            endRemoveRows();
        });
    }
}

void NetworkMediaModel::onItemPreparseEnded(MediaSourcePtr, input_item_node_t*, enum input_item_preparse_status)
{
    QMetaObject::invokeMethod(this, [this]() {
        m_parsingPending = false;
        emit parsingPendingChanged(false);
    });
}

void NetworkMediaModel::refreshMediaList( MediaSourcePtr mediaSource,
                                       input_item_node_t* const children[], size_t count,
                                       bool clear )
{
    auto items = new std::vector<Item>;
    for ( auto i = 0u; i < count; ++i )
    {
        auto it = children[i]->p_item;
        Item item;
        item.name = it->psz_name;
        item.protocol = "";
        item.indexed = false;
        item.type = static_cast<ItemType>(it->i_type);
        item.mainMrl = (item.type == TYPE_DIRECTORY || item.type == TYPE_NODE) ?
                    QUrl::fromEncoded(QByteArray(it->psz_uri).append('/')) :
                    QUrl::fromEncoded(it->psz_uri);

        item.canBeIndexed = canBeIndexed( item.mainMrl , item.type );
        item.mediaSource = mediaSource;

        if ( item.canBeIndexed == true )
        {
            if ( vlc_ml_is_indexed( m_ml, qtu( item.mainMrl.toString( QUrl::None ) ),
                                    &item.indexed ) != VLC_SUCCESS )
                item.indexed = false;
        }
        item.tree = NetworkTreeItem( mediaSource, it );
        items->push_back( std::move( item ) );
    }
    QMetaObject::invokeMethod(this, [this, clear, items]() {
        std::unique_ptr<std::vector<Item>> itemsPtr{ items };
        if ( clear == true )
        {
            beginResetModel();
            m_items.erase( begin( m_items ) , end( m_items ) );
        }
        else
            beginInsertRows( {}, m_items.size(), m_items.size() + items->size() - 1 );
        std::move( begin( *items ), end( *items ), std::back_inserter( m_items ) );
        if ( clear == true )
            endResetModel();
        else
            endInsertRows();
    });
}

bool NetworkMediaModel::canBeIndexed(const QUrl& url , ItemType itemType )
{
    return static_cast<input_item_type_e>(itemType) != ITEM_TYPE_FILE && (url.scheme() == "smb" || url.scheme() == "ftp" || url.scheme() == "file");
}
