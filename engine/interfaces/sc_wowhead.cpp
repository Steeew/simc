// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"

// source_str ===============================================================

static std::string source_str( wowhead::wowhead_e source )
{
  switch ( source )
  {
    case wowhead::PTR:  return "ptr";
    default:   return "www";
  }
}

static std::string source_desc_str( wowhead::wowhead_e source )
{
  switch ( source )
  {
    case wowhead::PTR:  return "PTR";
    default:   return "Live";
  }
}

// download_id ==============================================================

static std::shared_ptr<xml_node_t> download_id( sim_t*             sim,
                                unsigned           id,
                                cache::behavior_e  caching,
                                wowhead::wowhead_e source )
{
  if ( ! id )
    return std::shared_ptr<xml_node_t>();

  std::string url_www = "http://" + source_str( source ) + ".wowhead.com/item="
                        + util::to_string( id ) + "&xml";

  std::shared_ptr<xml_node_t> node = xml_node_t::get( sim, url_www, caching, "</json>" );
  if ( sim -> debug && node ) node -> print();
  return node;
}

// wowhead::download_glyph ==================================================

bool wowhead::download_glyph( player_t*          player,
                              std::string&       glyph_name,
                              const std::string& glyph_id,
                              wowhead_e          source,
                              cache::behavior_e  caching )
{
  unsigned glyphid = strtoul( glyph_id.c_str(), 0, 10 );
  std::shared_ptr<xml_node_t> node = download_id( player -> sim, glyphid, caching, source );
  if ( ! node || ! node -> get_value( glyph_name, "name/cdata" ) )
  {
    if ( caching != cache::ONLY )
      player -> sim -> errorf( "Unable to download glyph id %s from wowhead\n", glyph_id.c_str() );
    return false;
  }

  return true;
}

// download_item_data =======================================================

bool wowhead::download_item_data( item_t&            item,
                                  cache::behavior_e  caching,
                                  wowhead_e          source )
{
  std::shared_ptr<xml_node_t> xml = item.xml = download_id( item.sim, item.parsed.data.id, caching, source );

  if ( ! xml )
  {
    if ( caching != cache::ONLY )
      item.sim -> errorf( "Player %s unable to download item id '%u' from wowhead at slot %s.\n", item.player -> name(), item.parsed.data.id, item.slot_name() );
    return false;
  }

  try
  {
    int id;
    if ( ! xml -> get_value( id, "item/id" ) ) throw( "id" );
    item.parsed.data.id = id;

    if ( ! xml -> get_value( item.name_str, "name/cdata" ) ) throw( "name" );
    util::tokenize( item.name_str );

    xml -> get_value( item.icon_str, "icon/cdata" );

    if ( ! xml -> get_value( item.parsed.data.level, "level/." ) ) throw( "level" );

    std::string jsonequipdata, jsondata;
    xml -> get_value( jsonequipdata, "jsonEquip/cdata" );
    jsonequipdata = "{" + jsonequipdata + "}";
    xml -> get_value( jsondata, "json/cdata" );
    jsondata = "{" + jsondata + "}";

    js::js_node_t json = js::create( item.sim, jsondata );
    js::js_node_t jsonequip = js::create( item.sim, jsonequipdata );

    if ( item.sim -> debug )
      item.sim -> out_debug.raw() << json;

    if ( item.sim -> debug )
      item.sim -> out_debug.raw() << jsonequip;

    js::get_value( item.parsed.data.req_level, json, "reqlevel" );
    js::get_value( item.parsed.data.req_skill, jsonequip, "reqskill" );
    js::get_value( item.parsed.data.req_skill_level, jsonequip, "reqskillrank" );

    if ( ! xml -> get_value( item.parsed.data.quality, "quality/id" ) ) throw( "quality" );

    if ( ! js::get_value( item.parsed.data.inventory_type, json, "slot" ) ) throw( "inventory type" );
    if ( ! js::get_value( item.parsed.data.item_class, json, "classs" ) ) throw( "item class" );
    if ( ! js::get_value( item.parsed.data.item_subclass, json, "subclass" ) ) throw( "item subclass" );
    if ( item.parsed.data.item_subclass < 0 ) item.parsed.data.item_subclass = 0;

    // Todo binding type, needs htmlTooltip parsing
    if ( item.parsed.data.item_class == ITEM_CLASS_WEAPON )
    {
      int minDmg, maxDmg;
      double speed, dps;
      if ( ! js::get_value( dps, jsonequip, "dps" ) ) throw( "dps" );
      if ( ! js::get_value( speed, jsonequip, "speed" ) ) throw( "weapon speed" );
      if ( ! js::get_value( minDmg, jsonequip, "dmgmin1" ) ) throw( "weapon minimum damage" );
      if ( ! js::get_value( maxDmg, jsonequip, "dmgmax1" ) ) throw( "weapon maximum damage" );
      if ( dps && speed )
      {
        item.parsed.data.delay = speed * 1000.0;
        // Estimate damage range from the min damage blizzard gives us. Note that this is not exact, as the min dps is actually floored
        item.parsed.data.dmg_range = 2 - 2 * minDmg / ( dps * speed );
      }
    }

    int races = -1;
    js::get_value( races, jsonequip, "races" );
    item.parsed.data.race_mask = races;

    int classes = -1;
    js::get_value( classes, jsonequip, "classes" );
    item.parsed.data.class_mask = classes;

    std::vector<js::js_node_t> jsonequip_children = js::get_children( jsonequip );
    size_t n = 0;
    for ( size_t i = 0; i < jsonequip_children.size() && n < sizeof_array( item.parsed.data.stat_type_e ); i++ )
    {
      stat_e type = util::parse_stat_type( js::get_name( jsonequip_children[ i ] ) );
      if ( type == STAT_NONE || type == STAT_ARMOR || util::translate_stat( type ) == ITEM_MOD_NONE ) continue;

      item.parsed.data.stat_type_e[ n ] = util::translate_stat( type );
      js::get_value( item.parsed.data.stat_val[ n ], jsonequip_children[ i ] );
      n++;

      // Soo, weapons need a flag to indicate caster weapon for correct DPS calculation.
      if ( item.parsed.data.delay > 0 && ( item.parsed.data.stat_type_e[ i ] == ITEM_MOD_INTELLECT || item.parsed.data.stat_type_e[ i ] == ITEM_MOD_SPIRIT || item.parsed.data.stat_type_e[ i ] == ITEM_MOD_SPELL_POWER ) )
        item.parsed.data.flags_2 |= ITEM_FLAG2_CASTER_WEAPON;
    }

    int n_sockets = 0;
    js::get_value( n_sockets, jsonequip, "nsockets" );
    assert( n_sockets <= ( int ) sizeof_array( item.parsed.data.socket_color ) );
    char socket_str[32];
    for ( int i = 0; i < n_sockets; i++ )
    {
      snprintf( socket_str, sizeof( socket_str ), "socket%d", i + 1 );
      js::get_value( item.parsed.data.socket_color[ i ], jsonequip, socket_str );
    }

    js::get_value( item.parsed.data.id_set, jsonequip, "itemset" );
    js::get_value( item.parsed.data.id_socket_bonus, jsonequip, "socketbonus" );

    // Sad to the face
    std::string htmltooltip;
    xml -> get_value( htmltooltip, "htmlTooltip/cdata" );

    // Search for ">Heroic<", ">Raid Finder<", ">Heroic Thunderforged<" :/
    if ( util::str_in_str_ci( htmltooltip, ">Heroic<" ) || util::str_in_str_ci( htmltooltip, ">Heroic Thunderforged<" ) )
      item.parsed.data.type_flags |= RAID_TYPE_HEROIC;
    else if ( util::str_in_str_ci( htmltooltip, ">Raid Finder<" ) )
      item.parsed.data.type_flags |= RAID_TYPE_LFR;
    else if ( util::str_in_str_ci( htmltooltip, ">Flexible<" ) ) 
      item.parsed.data.type_flags |= RAID_TYPE_FLEXIBLE;

    if ( util::str_in_str_ci( htmltooltip, "Thunderforged<" ) )
      item.parsed.data.type_flags |= RAID_TYPE_ELITE;
    if ( util::str_in_str_ci( htmltooltip, "Warforged<" ) )
      item.parsed.data.type_flags |= RAID_TYPE_ELITE;
    if ( util::str_in_str_ci( htmltooltip, "Timeless<" ) )
      item.parsed.data.type_flags |= RAID_TYPE_ELITE;

    // Parse out Equip: and On use: strings
    int spell_idx = 0;

    std::shared_ptr<xml_node_t> htmltooltip_xml = xml_node_t::create( item.sim, htmltooltip );
    //htmltooltip_xml -> print( item.sim -> output_file, 2 );
    std::vector<xml_node_t*> spell_links = htmltooltip_xml -> get_nodes( "span" );
    for ( size_t i = 0; i < spell_links.size(); i++ )
    {
      int trigger_type = -1;
      unsigned spell_id = 0;

      std::string v;
      if ( spell_links[ i ] -> get_value( v, "." ) && v != "Equip: " && v != "Use: " )
        continue;

      if ( v == "Use: " )
        trigger_type = ITEM_SPELLTRIGGER_ON_USE;
      else if ( v == "Equip: " )
        trigger_type = ITEM_SPELLTRIGGER_ON_EQUIP;

      std::string url;
      if ( ! spell_links[ i ] -> get_value( url, "a/href" ) )
        continue;

      size_t begin = url.rfind( "=" );
      if ( begin == std::string::npos )
        continue;
      else
        begin++;

      spell_id = util::to_unsigned( url.substr( begin ) );
      if ( spell_id > 0 && trigger_type != -1 )
      {
        item.parsed.data.id_spell[ spell_idx ] = spell_id;
        item.parsed.data.trigger_spell[ spell_idx ] = trigger_type;
        spell_idx++;
      }
    }
  }
  catch ( const char* fieldname )
  {
    std::string error_str;

    xml -> get_value( error_str, "error/." );

    if ( caching != cache::ONLY )
      item.sim -> errorf( "Wowhead (%s): Player %s unable to parse item '%u' %s in slot '%s': %s\n",
                          source_desc_str( source ).c_str(), item.player -> name(), item.parsed.data.id,
                          fieldname, item.slot_name(), error_str.c_str() );
    return false;
  }

  return true;
}

// wowhead::download_item ===================================================

bool wowhead::download_item( item_t&            item,
                             wowhead_e          source,
                             cache::behavior_e  caching )
{
  bool ret = download_item_data( item, caching, source );

  if ( ret )
    item.source_str = "Wowhead";

  return ret;
}

std::string wowhead::decorated_action_name( const std::string& name,
                                            action_t* action,
                                            wowhead_e domain, 
                                            bool affix )
{
  std::string decorated_name, base_href, prefix, suffix, spell_name;
  unsigned spell_id = 0;

  // Kludge auto attack decorations
  if ( action -> type == ACTION_ATTACK )
  {
    attack_t* a = debug_cast< attack_t* >( action );
    if ( a -> auto_attack )
    {
      spell_id = 6603;
      if ( a -> weapon )
        spell_name = "Auto Attack";
    }
    else
    {
      spell_id = action -> id;
      if ( spell_id > 1 )
        spell_name = action -> s_data -> name_cstr();
    }
  }
  else
  {
    spell_id = action -> id;
    if ( spell_id > 1 )
      spell_name = action -> s_data -> name_cstr();
  }

  if ( spell_id > 1 )
  {
    base_href = "http://" + domain_str( domain ) + ".wowhead.com/spell=" + util::to_string( spell_id );

    if ( affix )
    {
      util::tokenize( spell_name );
      std::string::size_type affix_offset = name.find( spell_name );

      // Add an affix to the name, if the name does not match the 
      // spell name. Affix is either the prefix- or suffix portion of the 
      // non matching parts of the stats name.
      if ( affix_offset != std::string::npos && spell_name != name )
      {
        // Suffix
        if ( affix_offset == 0 )
          suffix += " (" + name.substr( spell_name.size() ) + ")";
        // Prefix
        else if ( affix_offset > 0 )
          prefix += " (" + name.substr( 0, affix_offset ) + ")";
      }
      else if ( affix_offset == std::string::npos )
        suffix += " (" + name + ")";
    }
  }

  if ( ! base_href.empty() )
  {
    if ( ! prefix.empty() )
      decorated_name += prefix + "&nbsp;";

    decorated_name += "<a href=\"" + base_href + "\">" + name + "</a>";

    if ( ! suffix.empty() )
      decorated_name += suffix;
  }
  else
    decorated_name = name;

  return decorated_name;
}

std::string wowhead::domain_str( wowhead_e domain )
{
  switch ( domain )
  {
    case PTR: return "ptr";
#if SC_BETA
    case BETA: return SC_BETA_STR;
#endif
    default: return "www";
  }
}
