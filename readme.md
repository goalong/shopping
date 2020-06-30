### 设计思路

对于秒杀的场景，由于库存一般比较有限，会出现大量的请求到达时已经售空的情况，如果每个请求都去数据库查一下库存，势必会给数据库带来很大的压力，大流量下数据库会首先扛不住。

这里是把库存提前放到redis里，每个请求到达时先判断库存是否大于0，库存小于等于0则请求直接返回已售罄。

如果此时有库存，则使用redis的INCRBY命令进行预扣库存，根据返回值判断操作是否成功：

* 返回值大于等于0，说明预扣成功，进行实际的数据库操作，包括下订单和减库存 (这里可以使用消息队列来处理，预扣成功后请求直接返回，由消息队列的消费者来处理后续的数据库操作)；

* 如果返回值小于0，说明预扣失败，库存已经不足，需要把预扣的再补回去。

### 存储

存储主要使用redis和mysql, redis进行查库存及预扣库存，mysql进行库存和订单的持久化存储。

商品库存使用redis的字符串类型，使用INCRBY来进行扣库存

另外使用一个集合类型来存储已购买该商品的用户ID，在判断是否重复购买时使用集合来判重

数据库主要是库存表和订单表，库存表存储商品ID和库存数量，订单表存储用户ID、商品ID、购买数量、创建时间等；

```
CREATE TABLE `item` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `stock` int(11) unsigned NOT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=22 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

```
CREATE TABLE `orders` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `item_id` int(11) NOT NULL,
  `uid` int(11) NOT NULL,
  `num` int(11) NOT NULL,
  `create_time` datetime NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=101 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```



