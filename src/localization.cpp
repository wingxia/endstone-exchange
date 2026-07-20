#include "endstone_exchange/localization.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace exchange {
namespace {

struct Translation {
    Message message;
    std::array<std::string_view, 4> text;
};

constexpr std::array Translations{
    Translation{Message::PluginNotReady,
                {"交易功能尚未就绪，请查看服务端日志。", "The exchange is not ready. Check the server log.",
                 "交易功能尚未就緒，請查看伺服器日誌。", "取引機能はまだ準備できていません。サーバーログを確認してください。"}},
    Translation{Message::CommandHelp,
                {"/exchange give | balance | orders | claim | addbalance <玩家> <金额> | status",
                 "/exchange give | balance | orders | claim | addbalance <player> <amount> | status",
                 "/exchange give | balance | orders | claim | addbalance <玩家> <金額> | status",
                 "/exchange give | balance | orders | claim | addbalance <プレイヤー> <金額> | status"}},
    Translation{Message::Balance,
                {"§6交易余额：§e{}§r", "§6Exchange balance: §e{}§r", "§6交易餘額：§e{}§r",
                 "§6取引残高：§e{}§r"}},
    Translation{Message::NoGivePermission,
                {"你没有发放 exchanger 的权限。", "You do not have permission to give an exchanger.",
                 "你沒有發放 exchanger 的權限。", "exchanger を配布する権限がありません。"}},
    Translation{Message::PlayerOffline,
                {"目标玩家不在线。", "The target player is offline.", "目標玩家不在線。",
                 "対象プレイヤーはオフラインです。"}},
    Translation{Message::GiveUsage,
                {"用法：/exchange give [在线玩家]", "Usage: /exchange give [online player]",
                 "用法：/exchange give [在線玩家]", "使い方：/exchange give [オンラインプレイヤー]"}},
    Translation{Message::NoMarketPermission,
                {"你没有使用 exchanger 开启交易的权限。", "You do not have permission to create an exchange point.",
                 "你沒有使用 exchanger 開啟交易的權限。", "exchanger で取引地点を作成する権限がありません。"}},
    Translation{Message::GaveExchanger,
                {"已向 {} 发放 exchanger。", "Gave an exchanger to {}.", "已向 {} 發放 exchanger。",
                 "{} に exchanger を渡しました。"}},
    Translation{Message::ConsoleHint,
                {"控制台请使用 addbalance 或 status。", "Use addbalance or status from the console.",
                 "控制台請使用 addbalance 或 status。", "コンソールでは addbalance または status を使用してください。"}},
    Translation{Message::PlayerFormOnly,
                {"该界面只能由玩家打开。", "Only a player can open this screen.", "該介面只能由玩家開啟。",
                 "この画面はプレイヤーのみ開けます。"}},
    Translation{Message::PlayerClaimOnly,
                {"只有玩家可以领取物品。", "Only a player can claim items.", "只有玩家可以領取物品。",
                 "アイテムを受け取れるのはプレイヤーだけです。"}},
    Translation{Message::AddBalanceUsage,
                {"用法：/exchange addbalance <在线玩家> <金额>",
                 "Usage: /exchange addbalance <online player> <amount>",
                 "用法：/exchange addbalance <在線玩家> <金額>",
                 "使い方：/exchange addbalance <オンラインプレイヤー> <金額>"}},
    Translation{Message::InvalidAmount,
                {"金额格式无效。", "The amount is invalid.", "金額格式無效。", "金額の形式が正しくありません。"}},
    Translation{Message::AmountOutOfRange,
                {"金额超出允许范围。", "The amount is outside the allowed range.", "金額超出允許範圍。",
                 "金額が許可範囲を超えています。"}},
    Translation{Message::PlayerBalanceNow,
                {"{} 的交易余额现在是 {}。", "{} now has an exchange balance of {}.",
                 "{} 的交易餘額現在是 {}。", "{} の取引残高は {} になりました。"}},
    Translation{Message::BalanceAdjusted,
                {"§6交易余额已调整为：§e{}§r", "§6Your exchange balance is now: §e{}§r",
                 "§6交易餘額已調整為：§e{}§r", "§6取引残高が次の金額に変更されました：§e{}§r"}},
    Translation{Message::StatusOk,
                {"Exchange {}：数据库正常，活跃交易点 {} 个", "Exchange {}: database OK, {} active exchange points",
                 "Exchange {}：資料庫正常，活躍交易點 {} 個", "Exchange {}：データベース正常、有効な取引地点 {} 件"}},
    Translation{Message::UnknownSubcommand,
                {"未知子命令。", "Unknown subcommand.", "未知子命令。", "不明なサブコマンドです。"}},
    Translation{Message::CommandFailed,
                {"交易操作失败：{}", "Exchange operation failed: {}", "交易操作失敗：{}", "取引操作に失敗しました：{}"}},
    Translation{Message::UnexpectedError,
                {"发生内部错误，请稍后重试或联系管理员。", "An internal error occurred. Try again later or contact an administrator.",
                 "發生內部錯誤，請稍後重試或聯絡管理員。", "内部エラーが発生しました。後でもう一度試すか、管理者に連絡してください。"}},
    Translation{Message::RecoveryCannotUse,
                {"该物品正在恢复，暂时不能使用。", "This item is being recovered and cannot be used yet.",
                 "該物品正在恢復，暫時不能使用。", "このアイテムは復元中のため、まだ使用できません。"}},
    Translation{Message::RecoveryCannotDrop,
                {"该物品正在恢复，暂时不能丢弃。", "This item is being recovered and cannot be dropped yet.",
                 "該物品正在恢復，暫時不能丟棄。", "このアイテムは復元中のため、まだ捨てられません。"}},
    Translation{Message::HologramNotTarget,
                {"交易提示不是可交易目标。", "The exchange label is not a trade target.",
                 "交易提示不是可交易目標。", "取引表示そのものは取引対象ではありません。"}},
    Translation{Message::BlockNoItem,
                {"该方块没有可交付的对应物品，不能设为可交易。",
                 "This block has no deliverable item and cannot become an exchange point.",
                 "該方塊沒有可交付的對應物品，不能設為可交易。",
                 "このブロックには渡せる対応アイテムがないため、取引地点にできません。"}},
    Translation{Message::ObjectEnabled,
                {"§a已将该物体设为可交易：§f{}§r", "§aThis object is now tradable: §f{}§r",
                 "§a已將該物體設為可交易：§f{}§r", "§aこの対象を取引可能にしました：§f{}§r"}},
    Translation{Message::ActivationFailed,
                {"开启交易失败：{}", "Could not create the exchange point: {}", "開啟交易失敗：{}",
                 "取引地点を作成できませんでした：{}"}},
    Translation{Message::FrameCanceled,
                {"§e已取消展示框的可交易设置。§r", "§eCanceled the item-frame exchange setup.§r",
                 "§e已取消展示框的可交易設定。§r", "§e額縁の取引設定をキャンセルしました。§r"}},
    Translation{Message::WorldNotLoaded,
                {"世界尚未加载。", "The world is not loaded yet.", "世界尚未載入。", "ワールドはまだ読み込まれていません。"}},
    Translation{Message::FrameReadFailed,
                {"无法读取展示框内容。", "Could not read the item frame.", "無法讀取展示框內容。",
                 "額縁の内容を読み取れませんでした。"}},
    Translation{Message::FrameReading,
                {"§e正在读取展示框内容，约 4 秒后完成设置；再次右击可取消。§r",
                 "§eReading the item frame. Setup takes about 4 seconds; right-click again to cancel.§r",
                 "§e正在讀取展示框內容，約 4 秒後完成設定；再次右擊可取消。§r",
                 "§e額縁を読み取っています。約4秒で設定されます。もう一度右クリックするとキャンセルできます。§r"}},
    Translation{Message::FrameMissing,
                {"展示框已不存在。", "The item frame no longer exists.", "展示框已不存在。", "額縁はすでに存在しません。"}},
    Translation{Message::FrameNoItem,
                {"展示框没有可交付的对应物品。", "The item frame has no deliverable item.",
                 "展示框沒有可交付的對應物品。", "額縁には渡せる対応アイテムがありません。"}},
    Translation{Message::FrameEnabled,
                {"§a已将展示框设为可交易：§f{}§r", "§aThe item frame is now tradable: §f{}§r",
                 "§a已將展示框設為可交易：§f{}§r", "§a額縁を取引可能にしました：§f{}§r"}},
    Translation{Message::FrameActivationFailed,
                {"开启展示框交易失败：{}", "Could not create the item-frame exchange point: {}",
                 "開啟展示框交易失敗：{}", "額縁の取引地点を作成できませんでした：{}"}},
    Translation{Message::ActorNoItem,
                {"该实体没有可交付的物品或刷怪蛋，不能设为可交易。",
                 "This entity has no deliverable item or spawn egg and cannot become an exchange point.",
                 "該實體沒有可交付的物品或生怪蛋，不能設為可交易。",
                 "このエンティティには渡せるアイテムやスポーンエッグがないため、取引地点にできません。"}},
    Translation{Message::ActorEnabled,
                {"§a已将该实体设为可交易：§f{}§r", "§aThis entity is now tradable: §f{}§r",
                 "§a已將該實體設為可交易：§f{}§r", "§aこのエンティティを取引可能にしました：§f{}§r"}},
    Translation{Message::MarketClosedPreserved,
                {"§e交易点已关闭；未完成订单和暂存的余额、物品保持不变，重新开启同一种物品后继续交易。§r",
                 "§eThe exchange point is closed. Open orders, reserved balance, and stored items are preserved and resume when the same item is enabled again.§r",
                 "§e交易點已關閉；未完成訂單和暫存的餘額、物品保持不變，重新開啟同一種物品後繼續交易。§r",
                 "§e取引地点を閉じました。未完了の注文、確保済み残高、保管アイテムは保持され、同じアイテムで再開すると取引を続けます。§r"}},
    Translation{Message::MarketRemoved,
                {"§e交易点已移除；未完成订单已取消，余额和物品已退回或等待领取。§r",
                 "§eThe exchange point was removed. Open orders were canceled; balance and items were returned or are waiting to be claimed.§r",
                 "§e交易點已移除；未完成訂單已取消，餘額和物品已退回或等待領取。§r",
                 "§e取引地点を削除しました。未完了の注文は取り消され、残高とアイテムは返却済み、または受け取り待ちです。§r"}},
    Translation{Message::MarketCloseFailed,
                {"关闭交易失败：{}", "Could not close the exchange point: {}", "關閉交易失敗：{}",
                 "取引地点を閉じられませんでした：{}"}},
    Translation{Message::TradePointClosed,
                {"这个交易点已经关闭。", "This exchange point is closed.", "這個交易點已經關閉。", "この取引地点は閉じられています。"}},
    Translation{Message::MatchingOrderGone,
                {"对应的订单已经没有了，请重新选择交易方式。",
                 "The matching orders are gone. Choose a trade method again.",
                 "對應的訂單已經沒有了，請重新選擇交易方式。",
                 "対応する注文がなくなりました。取引方法を選び直してください。"}},
    Translation{Message::BookWanted,
                {"§a有人要买§r\n", "§aPlayers want to buy§r\n", "§a有人要買§r\n", "§a購入希望§r\n"}},
    Translation{Message::BookForSale,
                {"§c有人在卖§r\n", "§cPlayers are selling§r\n", "§c有人在賣§r\n", "§c販売中§r\n"}},
    Translation{Message::TradeTitle,
                {"交易 · {}", "Exchange · {}", "交易 · {}", "取引 · {}"}},
    Translation{Message::CurrentTrade,
                {"当前交易", "Available orders", "目前交易", "現在の注文"}},
    Translation{Message::BalanceLabel,
                {"§6余额：§e{}§r", "§6Balance: §e{}§r", "§6餘額：§e{}§r", "§6残高：§e{}§r"}},
    Translation{Message::SellableRequirements,
                {"§6可出售：§e{} 件§r\n§6出售要求§r\n{}",
                 "§6You can sell: §e{}§r\n§6Item requirements§r\n{}",
                 "§6可出售：§e{} 件§r\n§6出售要求§r\n{}",
                 "§6販売可能：§e{} 個§r\n§6アイテム条件§r\n{}"}},
    Translation{Message::TradeOverviewAccount,
                {"§6余额§r §e{}§r  ·  §6可卖§r §e{} 件§r",
                 "§6Balance§r §e{}§r  ·  §6Sellable§r §e{}§r",
                 "§6餘額§r §e{}§r  ·  §6可賣§r §e{} 件§r",
                 "§6残高§r §e{}§r  ·  §6販売可能§r §e{} 個§r"}},
    Translation{Message::TradeOverviewRequirements,
                {"§6特殊要求§r", "§6Special requirements§r", "§6特殊要求§r", "§6特別な条件§r"}},
    Translation{Message::ChooseTradeAction,
                {"选择交易方式", "Choose how to trade", "選擇交易方式", "取引方法を選択"}},
    Translation{Message::DirectBuySubtitle,
                {"\n按当前出售价格购买", "\nBuy at current selling prices", "\n按目前出售價格購買",
                 "\n現在の販売価格で購入"}},
    Translation{Message::DirectSellSubtitle,
                {"\n按当前收购价格出售", "\nSell at current buying prices", "\n按目前收購價格出售",
                 "\n現在の購入価格で販売"}},
    Translation{Message::NoDirectBuy,
                {"当前没有人在出售，暂不显示直接购买。", "Nobody is selling now, so Buy now is hidden.",
                 "目前沒有人在出售，暫不顯示直接購買。", "現在販売中の人がいないため、「すぐ購入」は表示されません。"}},
    Translation{Message::NoDirectSell,
                {"当前没有人在收购，暂不显示直接出售。", "Nobody is buying now, so Sell now is hidden.",
                 "目前沒有人在收購，暫不顯示直接出售。", "現在購入希望者がいないため、「すぐ販売」は表示されません。"}},
    Translation{Message::OpenTradeFailed,
                {"打开交易界面失败：{}", "Could not open the exchange screen: {}", "開啟交易介面失敗：{}",
                 "取引画面を開けませんでした：{}"}},
    Translation{Message::QuantityInput,
                {"数量（1 至 {}）", "Quantity (1 to {})", "數量（1 至 {}）", "数量（1～{}）"}},
    Translation{Message::IntegerPlaceholder,
                {"请输入整数", "Enter a whole number", "請輸入整數", "整数を入力"}},
    Translation{Message::UnitPriceInput,
                {"每件价格（{} 至 {}，只填整数）", "Price per item ({} to {}, whole numbers only)",
                 "每件價格（{} 至 {}，只填整數）", "1個の価格（{}～{}、整数のみ）"}},
    Translation{Message::PricePlaceholder,
                {"例如 10", "For example, 10", "例如 10", "例：10"}},
    Translation{Message::ContinueButton, {"继续", "Continue", "繼續", "続ける"}},
    Translation{Message::InvalidFormData,
                {"客户端返回了无效表单数据", "The client returned invalid form data",
                 "客戶端返回了無效表單資料", "クライアントから無効なフォームデータが返されました"}},
    Translation{Message::InputInvalid,
                {"填写有误：{}。", "Invalid entry: {}.", "填寫有誤：{}。", "入力が正しくありません：{}。"}},
    Translation{Message::OpenInputFailed,
                {"打开交易填写界面失败：{}", "Could not open the trade entry screen: {}",
                 "開啟交易填寫介面失敗：{}", "取引入力画面を開けませんでした：{}"}},
    Translation{Message::ConfirmTitle,
                {"确认交易 · {}", "Confirm trade · {}", "確認交易 · {}", "取引確認 · {}"}},
    Translation{Message::QuantityValue,
                {"数量：{} 件", "Quantity: {}", "數量：{} 件", "数量：{} 個"}},
    Translation{Message::UnitPriceValue,
                {"每件价格：{}", "Price per item: {}", "每件價格：{}", "1個の価格：{}"}},
    Translation{Message::MarketBuyExplanation,
                {"将从当前最低出售价格开始购买，直到数量完成或没有可买物品。",
                 "Buys from the lowest current selling price until the quantity is filled or nothing remains for sale.",
                 "將從目前最低出售價格開始購買，直到數量完成或沒有可買物品。",
                 "現在の最安販売価格から、指定数量に達するか販売品がなくなるまで購入します。"}},
    Translation{Message::MarketSellExplanation,
                {"将从当前最高收购价格开始出售，直到数量完成或没有人继续收购。",
                 "Sells to the highest current buying price until the quantity is filled or nobody remains to buy.",
                 "將從目前最高收購價格開始出售，直到數量完成或沒有人繼續收購。",
                 "現在の最高購入価格から、指定数量に達するか購入希望がなくなるまで販売します。"}},
    Translation{Message::ReviewMaximumPayment,
                {"最多支付：{}", "Maximum payment: {}", "最多支付：{}", "支払い上限：{}"}},
    Translation{Message::ReviewExpectedIncome,
                {"预计收入：{}", "Expected income: {}", "預計收入：{}", "受取予定：{}"}},
    Translation{Message::ReviewDirectBuy,
                {"按当前最低出售价格依次购买，最多买入所填数量。",
                 "Buys from the lowest current selling price, up to the entered quantity.",
                 "按目前最低出售價格依次購買，最多買入所填數量。",
                 "現在の最安販売価格から、入力した数量まで購入します。"}},
    Translation{Message::ReviewDirectSell,
                {"按当前最高收购价格依次出售，最多卖出所填数量。",
                 "Sells to the highest current buying price, up to the entered quantity.",
                 "按目前最高收購價格依次出售，最多賣出所填數量。",
                 "現在の最高購入価格から、入力した数量まで販売します。"}},
    Translation{Message::AdjustQuantity,
                {"调整数量（1 至 {}）", "Adjust quantity (1 to {})", "調整數量（1 至 {}）", "数量を調整（1～{}）"}},
    Translation{Message::ReenterQuantity,
                {"填写数量（当前 {}）", "Enter quantity (currently {})", "填寫數量（目前 {}）",
                 "数量を入力（現在 {}）"}},
    Translation{Message::AdjustPrice,
                {"调整每件价格（{} 至 {}）", "Adjust price per item ({} to {})", "調整每件價格（{} 至 {}）",
                 "1個の価格を調整（{}～{}）"}},
    Translation{Message::ReenterPrice,
                {"填写价格（当前 {}）", "Enter price (currently {})", "填寫價格（目前 {}）", "価格を入力（現在 {}）"}},
    Translation{Message::ConfirmButton, {"§a确认交易§r", "§aConfirm trade§r", "§a確認交易§r", "§a取引を確定§r"}},
    Translation{Message::EditTradeButton,
                {"修改填写", "Edit entries", "修改填寫", "入力を修正"}},
    Translation{Message::BackActions,
                {"返回选择交易方式", "Back to trade methods", "返回選擇交易方式", "取引方法の選択に戻る"}},
    Translation{Message::OpenReviewFailed,
                {"打开交易确认界面失败：{}", "Could not open the confirmation screen: {}",
                 "開啟交易確認介面失敗：{}", "取引確認画面を開けませんでした：{}"}},
    Translation{Message::QuantityRangeInvalid,
                {"交易数量超出允许范围", "The trade quantity is outside the allowed range",
                 "交易數量超出允許範圍", "取引数量が許可範囲を超えています"}},
    Translation{Message::PriceRangeInvalid,
                {"交易价格超出允许范围，或不是整数", "The trade price is outside the allowed range or is not a whole number",
                 "交易價格超出允許範圍，或不是整數", "取引価格が許可範囲外、または整数ではありません"}},
    Translation{Message::QuantityField, {"数量", "Quantity", "數量", "数量"}},
    Translation{Message::UnitPriceField, {"每件价格", "Price per item", "每件價格", "1個の価格"}},
    Translation{Message::WholeNumberRange,
                {"{}必须是 {} 至 {} 的整数", "{} must be a whole number from {} to {}",
                 "{}必須是 {} 至 {} 的整數", "{}は {}～{} の整数で入力してください"}},
    Translation{Message::PriceTooLarge,
                {"每件价格超出允许范围", "The price per item is outside the allowed range",
                 "每件價格超出允許範圍", "1個の価格が許可範囲を超えています"}},
    Translation{Message::PriceStep,
                {"每件价格必须按 {}u 调整", "The price per item must change in steps of {}u",
                 "每件價格必須按 {}u 調整", "1個の価格は {}u 単位で調整してください"}},
    Translation{Message::DeliveryReceived,
                {"，已到账 {} 件", ", {} item(s) received", "，已到帳 {} 件", "、{} 個を受け取りました"}},
    Translation{Message::DeliveryReturned,
                {"，已返还 {} 件", ", {} item(s) returned", "，已返還 {} 件", "、{} 個が返却されました"}},
    Translation{Message::DeliveryPending,
                {"，另有物品待 /exchange claim 领取", ", more items are waiting for /exchange claim",
                 "，另有物品待 /exchange claim 領取", "、ほかのアイテムは /exchange claim で受け取れます"}},
    Translation{Message::OtherPlayerSelling,
                {"出售这种物品", "selling this item", "出售這種物品", "このアイテムを販売している人"}},
    Translation{Message::OtherPlayerBuying,
                {"收购这种物品", "buying this item", "收購這種物品", "このアイテムを購入希望の人"}},
    Translation{Message::OrderNoFill,
                {"§e订单 #{} 未完成：当前没有其他玩家{}；自己的订单不会和自己交易{}。余额 {}。§r",
                 "§eOrder #{} was not filled: no other player is currently {}; your own orders cannot trade with each other{}. Balance {}.§r",
                 "§e訂單 #{} 未完成：目前沒有其他玩家{}；自己的訂單不會和自己交易{}。餘額 {}。§r",
                 "§e注文 #{} は成立しませんでした。現在、他のプレイヤーで{}はいません。自分の注文同士は取引されません{}。残高 {}。§r"}},
    Translation{Message::BuyVerb, {"购买", "buy", "購買", "購入"}},
    Translation{Message::SellVerb, {"出售", "sell", "出售", "販売"}},
    Translation{Message::OrderSaved,
                {"§a订单 #{} 已保存：等待按每件 {} {} {} 件{}。余额 {}。§r",
                 "§aOrder #{} saved: waiting at {} each to {} {} item(s){}. Balance {}.§r",
                 "§a訂單 #{} 已儲存：等待按每件 {} {} {} 件{}。餘額 {}。§r",
                 "§a注文 #{} を保存しました：1個 {} で {} を {} 個待機中{}。残高 {}。§r"}},
    Translation{Message::OrderPartiallyFilled,
                {"§a订单 #{} 已完成 {} 件，剩余 {} 件继续等待，总金额 {}{}。余额 {}。§r",
                 "§aOrder #{} filled {} item(s); {} remain open. Total {}{}. Balance {}.§r",
                 "§a訂單 #{} 已完成 {} 件，剩餘 {} 件繼續等待，總金額 {}{}。餘額 {}。§r",
                 "§a注文 #{} は {} 個成立し、残り {} 個は継続待機です。合計 {}{}。残高 {}。§r"}},
    Translation{Message::TradeCompleted,
                {"§a交易完成：订单 #{} 共 {} 件，总金额 {}{}。余额 {}。§r",
                 "§aTrade complete: order #{} filled {} item(s), total {}{}. Balance {}.§r",
                 "§a交易完成：訂單 #{} 共 {} 件，總金額 {}{}。餘額 {}。§r",
                 "§a取引完了：注文 #{}、{} 個、合計 {}{}。残高 {}。§r"}},
    Translation{Message::OrderEnded,
                {"§a订单 #{} 已结束：完成 {}/{} 件，总金额 {}{}。余额 {}。§r",
                 "§aOrder #{} ended: {}/{} item(s) filled, total {}{}. Balance {}.§r",
                 "§a訂單 #{} 已結束：完成 {}/{} 件，總金額 {}{}。餘額 {}。§r",
                 "§a注文 #{} は終了しました：{}/{} 個成立、合計 {}{}。残高 {}。§r"}},
    Translation{Message::TradeFailed,
                {"交易失败：{}", "Trade failed: {}", "交易失敗：{}", "取引に失敗しました：{}"}},
    Translation{Message::OrdersTitle,
                {"我的未完成订单", "My open orders", "我的未完成訂單", "自分の未完了注文"}},
    Translation{Message::OrdersHeader,
                {"点击订单即可取消", "Select an order to cancel it", "點擊訂單即可取消", "注文を選ぶとキャンセルできます"}},
    Translation{Message::NoOpenOrders,
                {"当前没有未完成订单。", "You have no open orders.", "目前沒有未完成訂單。", "未完了の注文はありません。"}},
    Translation{Message::OrderButton,
                {"#{} {} {} × {}\n点击取消", "#{} {} {} × {}\nSelect to cancel",
                 "#{} {} {} × {}\n點擊取消", "#{} {} {} × {}\n選択してキャンセル"}},
    Translation{Message::BuyColored, {"§a购买§r", "§aBuy§r", "§a購買§r", "§a購入§r"}},
    Translation{Message::SellColored, {"§c出售§r", "§cSell§r", "§c出售§r", "§c販売§r"}},
    Translation{Message::OrderCanceled,
                {"§a订单 #{} 已取消。§r", "§aOrder #{} canceled.§r", "§a訂單 #{} 已取消。§r",
                 "§a注文 #{} をキャンセルしました。§r"}},
    Translation{Message::CancelOrderFailed,
                {"取消订单失败：{}", "Could not cancel the order: {}", "取消訂單失敗：{}",
                 "注文をキャンセルできませんでした：{}"}},
    Translation{Message::ReadOrdersFailed,
                {"读取订单失败：{}", "Could not load orders: {}", "讀取訂單失敗：{}", "注文を読み込めませんでした：{}"}},
    Translation{Message::ClaimedItems,
                {"§a已领取 {} 件交易物品。§r", "§aClaimed {} exchange item(s).§r",
                 "§a已領取 {} 件交易物品。§r", "§a取引アイテムを {} 個受け取りました。§r"}},
    Translation{Message::ExchangerLore,
                {"§7右击物体：开启/关闭交易", "§7Right-click an object: enable/disable exchange",
                 "§7右擊物體：開啟/關閉交易", "§7対象を右クリック：取引を開始/終了"}},
    Translation{Message::ExchangerMetaFailed,
                {"无法设置 exchanger 物品名称", "Could not set the exchanger item name",
                 "無法設定 exchanger 物品名稱", "exchanger のアイテム名を設定できませんでした"}},
    Translation{Message::ExchangerReceived,
                {"§a已获得 exchanger。§r", "§aYou received an exchanger.§r", "§a已獲得 exchanger。§r",
                 "§aexchanger を受け取りました。§r"}},
    Translation{Message::ReceiptName,
                {"§r交易暂存记录", "§rExchange recovery record", "§r交易暫存記錄", "§r取引復元記録"}},
    Translation{Message::ReceiptLore,
                {"§7请勿移动；系统将自动回收", "§7Do not move; the system will recover it automatically",
                 "§7請勿移動；系統將自動回收", "§7移動しないでください。システムが自動的に回収します"}},
    Translation{Message::HologramWanted, {"收购", "Buying", "收購", "購入希望"}},
    Translation{Message::HologramForSale, {"出售", "Selling", "出售", "販売中"}},
    Translation{Message::TradeActionLimitBuy,
                {"按我的价格购买", "Buy at my price", "按我的價格購買", "希望価格で購入"}},
    Translation{Message::TradeActionLimitSell,
                {"按我的价格出售", "Sell at my price", "按我的價格出售", "希望価格で販売"}},
    Translation{Message::TradeActionMarketBuy, {"直接购买", "Buy now", "直接購買", "すぐ購入"}},
    Translation{Message::TradeActionMarketSell, {"直接出售", "Sell now", "直接出售", "すぐ販売"}},
    Translation{Message::TradeActionFallback, {"交易", "Trade", "交易", "取引"}},
    Translation{Message::ItemEmptyText, {"空文本", "empty text", "空文字", "空のテキスト"}},
    Translation{Message::ItemEmpty, {"空", "empty", "空", "空"}},
    Translation{Message::ItemCountSuffix,
                {"，共 {} 项", ", {} entries total", "，共 {} 項", "、全 {} 項目"}},
    Translation{Message::ItemDeepValue,
                {"{}：内容层级较深，请使用同来源物品", "{}: nested data is deep; use an item from the same source",
                 "{}：內容層級較深，請使用同來源物品", "{}：データ階層が深いため、同じ入手元のアイテムを使用してください"}},
    Translation{Message::ItemListEntry,
                {"{} 第 {} 项", "{} entry {}", "{} 第 {} 項", "{} の第 {} 項目"}},
    Translation{Message::ItemLabel, {"物品：{}", "Item: {}", "物品：{}", "アイテム：{}"}},
    Translation{Message::ItemData, {"数据值：{}", "Data value: {}", "資料值：{}", "データ値：{}"}},
    Translation{Message::NoneValue, {"无", "none", "無", "なし"}},
    Translation{Message::ItemCustomName,
                {"自定义名称：{}", "Custom name: {}", "自訂名稱：{}", "カスタム名：{}"}},
    Translation{Message::ItemLore, {"物品说明：{}", "Description: {}", "物品說明：{}", "説明：{}"}},
    Translation{Message::ItemEnchantments,
                {"附魔：{}", "Enchantments: {}", "附魔：{}", "エンチャント：{}"}},
    Translation{Message::ItemDurabilityDamage,
                {"耐久损耗：{}", "Durability damage: {}", "耐久損耗：{}", "耐久値の消耗：{}"}},
    Translation{Message::ItemRepairCost,
                {"铁砧修复代价：{}", "Anvil repair cost: {}", "鐵砧修復代價：{}", "金床修理コスト：{}"}},
    Translation{Message::ItemUnbreakable,
                {"不会损坏：{}", "Unbreakable: {}", "不會損壞：{}", "破壊不能：{}"}},
    Translation{Message::NoValue, {"否", "no", "否", "いいえ"}},
    Translation{Message::YesValue, {"是", "yes", "是", "はい"}},
    Translation{Message::ItemOtherNone,
                {"其他属性：无", "Other properties: none", "其他屬性：無", "その他の属性：なし"}},
    Translation{Message::ItemOtherHeader,
                {"其他属性：", "Other properties:", "其他屬性：", "その他の属性："}},
    Translation{Message::ItemOtherMany,
                {"- 属性内容很多；未列出的部分也必须相同",
                 "- There are many properties; unlisted properties must also match",
                 "- 屬性內容很多；未列出的部分也必須相同",
                 "- 属性が多いため、表示されていない部分も同じである必要があります"}},
    Translation{Message::UnknownEnchantment,
                {"未知附魔编号 {}", "Unknown enchantment ID {}", "未知附魔編號 {}", "不明なエンチャントID {}"}},
    Translation{Message::InventoryInsufficient,
                {"背包中有 {} 件 {}。符合出售要求的有 {} 件，本次需要 {} 件。{}",
                 "Your inventory contains {} {} item(s). {} match the selling requirements; {} are needed.{}",
                 "背包中有 {} 件 {}。符合出售要求的有 {} 件，本次需要 {} 件。{}",
                 "インベントリには {} 個の {} があります。販売条件に合うものは {} 個で、今回は {} 個必要です。{}"}},
};

constexpr std::size_t languageIndex(const Language language) noexcept {
    return static_cast<std::size_t>(language);
}

constexpr std::size_t placeholderCount(const std::string_view value) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '{') {
            continue;
        }
        if (index + 1 < value.size() && value[index + 1] == '{') {
            ++index;
            continue;
        }
        ++count;
    }
    return count;
}

constexpr bool complete() noexcept {
    std::array<unsigned, static_cast<std::size_t>(Message::Count)> counts{};
    for (const auto &entry : Translations) {
        const auto index = static_cast<std::size_t>(entry.message);
        if (index >= counts.size()) {
            return false;
        }
        ++counts[index];
        for (const auto value : entry.text) {
            if (value.empty() || placeholderCount(value) != placeholderCount(entry.text.front())) {
                return false;
            }
        }
    }
    return std::ranges::all_of(counts, [](const unsigned count) { return count == 1; });
}

static_assert(complete(), "every message must have exactly one translation in all supported languages");

} // namespace

Language languageFromLocale(std::string_view locale) noexcept {
    std::string normalized(locale);
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char value) {
        return value == '-' ? '_' : static_cast<char>(std::tolower(value));
    });
    if (normalized.starts_with("zh_tw") || normalized.starts_with("zh_hk") || normalized.starts_with("zh_mo") ||
        normalized == "zh_hant") {
        return Language::TraditionalChinese;
    }
    if (normalized.starts_with("zh")) {
        return Language::SimplifiedChinese;
    }
    if (normalized.starts_with("en")) {
        return Language::English;
    }
    if (normalized.starts_with("ja")) {
        return Language::Japanese;
    }
    return Language::SimplifiedChinese;
}

std::string_view localeCode(const Language language) noexcept {
    switch (language) {
    case Language::SimplifiedChinese:
        return "zh_CN";
    case Language::English:
        return "en_US";
    case Language::TraditionalChinese:
        return "zh_TW";
    case Language::Japanese:
        return "ja_JP";
    }
    return "zh_CN";
}

std::string_view messageText(const Language language, const Message message) noexcept {
    const auto found = std::ranges::find_if(Translations, [message](const auto &entry) {
        return entry.message == message;
    });
    if (found == Translations.end()) {
        return "missing translation";
    }
    return found->text[languageIndex(language)];
}

bool translationCatalogComplete() noexcept {
    return complete();
}

std::string userFacingError(const Language language, const std::exception &error) {
    if (dynamic_cast<const UserError *>(&error) != nullptr) {
        return error.what();
    }
    return tr(language, Message::UnexpectedError);
}

} // namespace exchange
