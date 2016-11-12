git rev-list --reverse --topo-order 1a751d3574c3105b6de67c8946d4a92bc42920b0^..14b2a98123ed2538d163ca067935ee1bea572e3e | while read rev 
do 
  git cherry-pick $rev || break 
done 
